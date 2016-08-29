#include "searcher.h"
#include <database.h>
#include <soci/soci.h>
#include <database_query_table.h>
#include <QDebug>
#include "search_cache.h"
#include <pref.h>

using std::string;
using std::make_shared;

SearchCache SearcherFab::cache_;

Searcher::Searcher(ptree r)
	: req_(r)
{
	ans_.put("cat", "search result");
}

Searcher::~Searcher()
{
}

void Searcher::set_unique_key(const uint256_t& key)
{
	ans_.put("cache-cookie", uint256_to_hex(key));
}

void Searcher::page(ptree pt)
{
	req_.put("start", pt.get<ssize_t>("start", 0));
	req_.put("number", pt.get<ssize_t>("number", 0));
}

class NoGoSearcher : public Searcher {
public:
	NoGoSearcher(const char* reason)
		:Searcher(ptree()), reason_(reason)
	{
	}

	ptree do_search() override
	{
		ans_.put("status", reason_);
		return ans_;
	}
private:
	const char* reason_;
};

struct FileResult;
struct NumberOfResults {
	ssize_t number;
	enum {
		UNKNOWN,
		PARTIAL,
		FINAL
	} status;

	NumberOfResults()
		:number(0), status(UNKNOWN)
	{
	};
};

class RegexSearcher : public Searcher {
public:
	using Searcher::Searcher;

	ptree do_search() override;
private:
	void skip_vol_up_to(long long volid,
			ssize_t skip_up_to,
                        const string& pattern,
			ssize_t& nrecord,
			bool& done);
	ssize_t search_vol(long long volid,
                        const string& mount,
                        const string& pattern,
                        ssize_t start,
                        ssize_t limit,
                        ptree result_array,
			bool drop = false);
	std::vector<FileResult> file_results_; // FIXME: batch query
	// Cached number of results per volume
	// a negative value means final result
	std::map<int, NumberOfResults> known_nresults_;
};

std::shared_ptr<Searcher> SearcherFab::fab(ptree pt)
{
	string tmp;
	pt.dump_to(tmp);
	qDebug() << "Incoming search request: " << tmp.c_str();

	qDebug() << "Incoming search class : " << pt.get<string>("class", "").c_str();
	if (pt.get<string>("cat", "") != "byname")
		return make_shared<NoGoSearcher>("Unsupported search class.");

	if (pt.get("matcher", "") != "regex")
		return make_shared<NoGoSearcher>("Only 'regex' is supported.");

	if (pt.get("pattern", "").empty())
		return make_shared<NoGoSearcher>("'pattern' cannot be empty.");

	if (pt.get<ssize_t>("start", -1) < 0)
		pt.put<ssize_t>("start", 0);
	int limits = Pref::instance()->get_registry().get("searcher.limits", 100);
	if (pt.get<ssize_t>("number", limits + 1) > limits)
		pt.put<ssize_t>("number", limits);

	auto cached = cache_.find(pt);
	if (cached) {
		cached->page(pt);
		return cached;
	}

	auto searcher = make_shared<RegexSearcher>(pt);
	searcher->set_unique_key(cache_.cache(searcher));
	return searcher;
}

struct FileResult {
	// Required
	string name;
	uint64_t size;
	string path;
	long long mtime_sec;
	long long mtime_nsec;

	// Cached
	uint64_t inode;
};

ptree
RegexSearcher::do_search()
{
	string pattern = get<string>("pattern", "");
	auto dbc = DatabaseRegistry::get_shared_dbc();

	// FIXME: invalid the cache when mounted volumes changed
	soci::rowset<soci::row> indexed = (dbc->prepare <<
R"zzz(
SELECT trID, volumes_table.uuid, mount 
ORDER BY trID
FROM tracking_table LEFT JOIN volumes_table ON (tracking_table.uuid = volumes_table.uuid);
)zzz"
);
	ans_.put("result", "OK");
	try {
		ssize_t off = req_.get<ssize_t>("start", 0);
		ssize_t limit = req_.get<ssize_t>("number", 0);
		ssize_t cursor = 0;
		ptree array;
		for (auto& row : indexed) {
			auto trID = row.get<int>(0);
			ssize_t start_this_vol = 0;
			if (cursor < off) {
				ssize_t nrecord = 0;
				bool done = false;
				skip_vol_up_to(trID, off - cursor, pattern, nrecord, done);
				cursor += nrecord;
				if (done)
					continue;
				start_this_vol = nrecord;
			}

			auto mount = row.get<string>(2);
			ssize_t nrow = search_vol(trID,
					          mount,
				                  pattern,
				                  start_this_vol,
				                  limit,
						  array
						 );
			cursor += nrow;
			limit -= nrow;
			if (limit <= 0)
				break;
		}
		ans_.swap_child_with("items", array);
	} catch(std::exception& e) {
		ans_.put("result", "Error");
		ans_.put("reason", e.what());
	}
	return ans_;
}

void
RegexSearcher::skip_vol_up_to(long long volid,
		ssize_t skip_up_to,
                const string& pattern,
		ssize_t& nrecord,
		bool& done)
{
	NumberOfResults volrecord = known_nresults_[volid];
	auto status = volrecord.status;
	auto number = volrecord.number;
	if (status != NumberOfResults::FINAL) {
		if (number <= skip_up_to) {
			nrecord = number;
			done = true;
		} else {
			nrecord = skip_up_to;
			done = false;
		}
		return ;
	}
	nrecord = 0;
	if (status == NumberOfResults::PARTIAL) {
		if (number >= skip_up_to) {
			// Known record fulfill our skipping requirements
			nrecord = skip_up_to;
			done = false;
			return ;
		}
		// Otherwise, skip known records
		// and treate the remaining as UNKNOWN.
		nrecord = number;
		skip_up_to -= number;
	}
	nrecord += search_vol(volid, std::string(), pattern, nrecord, skip_up_to, ptree(), true);
	done = (known_nresults_[volid].status == NumberOfResults::FINAL);
}

ssize_t
RegexSearcher::search_vol(long long volid,
                          const string& mount,
                          const string& pattern,
                          ssize_t start,
                          ssize_t limit,
                          ptree result_array,
			  bool drop)
{
	if (volid < 0)
		return -1;
	auto dbc = DatabaseRegistry::get_shared_dbc();
	auto sqlprovider = DatabaseRegistry::get_sql_provider();
	
	soci::rowset<soci::row> indexed = (dbc->prepare <<
			sqlprovider->query_volume(volid, query::volume::REGEX_NAME_MATCH),
			soci::use(pattern),
			soci::use(limit),
			soci::use(start)
		);
	ssize_t nrecord = 0;
	if (!drop) {
		std::string volstr = std::to_string(volid);
		for(const auto& row: indexed) {
			ptree item;
			item.put("name", row.get<string>(0));
			if (mount.empty()) 
				item.put("path", volstr + row.get<string>(1));
			else
				item.put("path", mount + row.get<string>(1));
			result_array.push_back(std::move(item));
			nrecord++;
		}
	} else {
		for(const auto& row: indexed)
			nrecord++;
	}
	auto& record = known_nresults_[volid];
	if (nrecord > 0 || start <= record.number) { // Valid window
		ssize_t new_number = start + nrecord;
		record.number = std::max(record.number, new_number);
		if (nrecord < limit)
			record.status = NumberOfResults::FINAL;
	}
	return nrecord;
}
