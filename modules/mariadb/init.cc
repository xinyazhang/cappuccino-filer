/*
 * Demo 0: Basic module
 *
 * The module loader will check every shared library under specified path(s),
 * and then check the existence of entrance functions: draft_module_init and
 * draft_module_term
 *
 * After that the module loader will call draft_module_init. A successful
 * loaded module should return a non-negative integer.
 *
 * On program exit, the loader will call draft_module_term.
 *
 */
#include <json.h>
#include <pref.h>
#include <database.h>
#include <string>
#include <memory>
#include <QDebug>
#include <sqlpp11/sqlpp11.h>
#include <sqlpp11/mysql/mysql.h>

using std::string;
namespace mysql = sqlpp::mysql;

extern "C" {

int draft_module_init()
{
	auto config = std::make_shared<mysql::connection_config>();
	auto reg = Pref::instance()->get_registry();
	try {
		config->user = reg->get<string>("mariadb.user");
		config->password = reg->get<string>("mariadb.password");
		config->database = reg->get<string>("mariadb.database");
		config->debug = true;
	} catch (boost::property_tree::ptree_bad_path& e) {
		qDebug() << " Unable to access perference " << e.what();
	}
	mysql::connection *dbptr = nullptr;
	try 
	{
		dbptr = new mysql::connection(config);
	}
	catch (const sqlpp::exception&)
	{
		std::cerr << "For testing, you'll need to create a database sqlpp_mysql with a table tab_sample, as shown in "
			"tests/TabSample.sql" << std::endl;
		return -1;
	}
	DatabaseRegistry::register_database(dbptr);
	if (reg->get<bool>("mariadb.debug", false)) {
		dbptr->execute("DROP TABLE IF EXISTS tab_volumes");
	}
	return 0;
}

int draft_module_term()
{
	DatabaseRegistry::close_database();
}

};
