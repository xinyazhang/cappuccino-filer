#include "volume.h"
#include "pref.h"
#include <database.h>
#include <string>
#include <json.h>
#include <launcher.h>
#include <QDebug>
#include <QtCore/QProcess>
#include <soci/soci.h>
#include <boost/lexical_cast.hpp>

using soci::use;
using soci::into;
using std::cout;

Volume* Volume::instance()
{
	static Volume volume;
	return &volume;
}

Volume::Volume()
{
	auto& db = *(DatabaseRegistry::get_shared_dbc());
	soci::transaction tr(db);
	db << R"(CREATE TABLE IF NOT EXISTS volumes_table(
				uuid char(40) PRIMARY KEY,
				label char(32),
				mount text
			);)";
	db << R"(CREATE TABLE IF NOT EXISTS tracking_table(
				trID int PRIMARY KEY AUTO_INCREMENT,
				uuid char(40) UNIQUE,
				tracking int NOT NULL DEFAULT 0,
				FOREIGN KEY(uuid) REFERENCES volumes_table(uuid)
			);)";
	tr.commit();
}

void Volume::scan(DbConnection dbc)
{
	qDebug() << "Volume::scan " << dbc.get() ;
	QProcess lsblk;
	lsblk.start("lsblk", QStringList() << "--json" << "--fs" << "--paths" << "--list");

	if (!lsblk.waitForFinished()) {
		    qWarning() << "lsblk failed:" << lsblk.errorString();
			return;
	}
	auto all = lsblk.readAll();
	//qDebug() << "lsblk output:" << all;
	std::stringstream ss(all.toStdString());
	ptree pt;
	json_read_from_stream(ss, pt);

	soci::transaction tr(*dbc);
	for(const auto& iter: pt.get_child("blockdevices")) {
		const auto& sub = iter.second;
		qDebug() << "lsblk item:" << sub.get<std::string>("uuid", "failed").c_str();
		auto mp = sub.get<std::string>("mountpoint", "");
		if (mp[0] != '/')
			continue;
		try {
			*dbc << "INSERT INTO volumes_table(uuid, label, mount) VALUES(:1, :2, :3) ON DUPLICATE KEY UPDATE mount=VALUES(mount)",
				use(sub.get("uuid", "failed")),
				use(sub.get("label", "")),
				use(mp);
		} catch (...) {
			qWarning() << "Exception thrown during inserting ";
		}
	}
	tr.commit();
	qWarning() << "Commited";
}

shared_ptree Volume::ls_volumes()
{
	shared_ptree ret = create_ptree();
	auto dbc = DatabaseRegistry::get_shared_dbc();
	//scan(dbc);
	try {
		soci::rowset<soci::row> mpoints = (dbc->prepare <<
				"SELECT volumes_table.uuid, mount, CASE WHEN tracking_table.trID IS NOT NULL THEN tracking_table.tracking ELSE 0 END AS tracking FROM volumes_table LEFT JOIN tracking_table ON (volumes_table.uuid = tracking_table.uuid);");
		ptree content;
		for(auto& row : mpoints) {
			ptree vol;
			vol.put("uuid", row.get<string>(0));
			vol.put("mount", row.get<string>(1));
			auto value = row.get<long long>(2);
			vol.put("tracking", value != 0);
			std::string tmp;
			json_write_to_string(vol, tmp);
			content.push_back(std::make_pair("", vol));
		}
		ret->add_child("volumelist", content);
	} catch (std::exception& e) {
		qDebug() << e.what();
	}
#if 0
	std::string tmp;
	json_write_to_string(ret, tmp);
	qDebug() << "Volumes: " << tmp.c_str();
#endif
	return ret;
}

// FIXME: acutal handle something
shared_ptree Volume::handle_request(shared_ptree pt)
{
	std::vector<std::string> path_list;
	try {
		auto dbc = DatabaseRegistry::get_shared_dbc();
		soci::transaction tr1(*dbc);
		for(const auto& kvpair: pt->get_child("volumelist")) {
			// Retrive volume from ptree
			const auto& vol = kvpair.second;
			auto uuid = vol.get<std::string>("uuid", "");
			auto mp = vol.get<std::string>("mount", "");
			auto tracking_str = vol.get<std::string>("tracking", "");
			qDebug() << "request volume item:" << uuid.c_str() << "\t" << mp.c_str() << "\t" << tracking_str.c_str();
			bool tracking;
			std::istringstream(tracking_str) >> std::boolalpha >> tracking;

			// Check the old state
			int current_tracking = -1;
			(*dbc) << "SELECT tracking FROM tracking_table WHERE uuid = :uuid",
				into(current_tracking), use(uuid);
			if (current_tracking == -1)
				current_tracking = 0; // Non-existing equals to false
			if (!!current_tracking == tracking)
				continue ; // State not changed, continue to the next
			qDebug() << "Changing volume :" << uuid.c_str() << "\t" << mp.c_str();

			if (!tracking) {
				// Sync the state
				// Note: setting non-tracking -> tracking should be
				// done by updatedb
				*dbc << "INSERT INTO tracking_table(uuid, tracking) VALUES(:1, :2) ON DUPLICATE KEY UPDATE tracking = :3",
					use(uuid),
					use((int)tracking),
					use((int)tracking);
				continue; 
			}

			if (mp.empty()) {
				qDebug() << "Cannot initiating tracking on Volume "
					<< uuid.c_str()
					<< ": volume was not mounted.";
				continue;
			}
			path_list.emplace_back(mp);
		}
		tr1.commit();
	} catch (soci::soci_error& e) {
		qDebug() << e.what();
	} catch (std::exception& e) {
		qDebug() << e.what();
	} catch (...) {
		std::string buf;
		json_write_to_string(pt, buf);
		qDebug() << "Error in parsing json " << buf.c_str();
	}
	if (!path_list.empty()) {
		ptree paths;
		for(const auto& mp : path_list) {
			ptree path;
			path.put("", mp);
			paths.push_back(std::make_pair("", path));
		}
		// Launch the updatedb process
		shared_ptree req = create_ptree();
		req->add_child("paths", paths);
		std::string buf;
		json_write_to_string(req, buf);
		qDebug() << "Paths " << buf.c_str();
		Launcher::instance()->launch(Pref::instance()->get_pref("core.libexecpath")+"updatedb", req);
	}
	return ls_volumes();
}
