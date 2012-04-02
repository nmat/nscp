#include "StdAfx.h"

#include <nsclient/logger.hpp>
#include <nsclient/base_logger_impl.hpp>
#include "logger_impl.hpp"

#include <concurrent_queue.hpp>

#include <nscapi/functions.hpp>
#include <nscapi/nscapi_helper.hpp>

#include "../helpers/settings_manager/settings_manager_impl.h"

#include <settings/client/settings_client.hpp>


nsclient::logging::impl::raw_subscribers subscribers_;

void log_fatal(std::string message) {
	std::cout << message << std::endl;
}

std::string create_message(Plugin::LogEntry::Entry::Level level, const char* file, const int line, std::wstring logMessage) {
	std::string str;
	try {
		Plugin::LogEntry message;
		Plugin::LogEntry::Entry *msg = message.add_entry();
		msg->set_level(level);
		msg->set_file(file);
		msg->set_line(line);
		msg->set_message(utf8::cvt<std::string>(logMessage));
		return message.SerializeAsString();
	} catch (std::exception &e) {
		log_fatal(std::string("Failed to generate message: ") + e.what());
	} catch (...) {
		log_fatal("Failed to generate message: <UNKNOWN>");
	}
	return str;
}
std::string nsclient::logging::logger_helper::create(NSCAPI::log_level::level level, const char* file, const int line, std::wstring message) {
	return create_message(nscapi::functions::log_to_gpb(level), file, line, message);
}

std::wstring render_log_level_short(Plugin::LogEntry::Entry::Level l) {
	return nsclient::logging::logger_helper::render_log_level_short(nscapi::functions::gpb_to_log(l));
}

std::wstring render_log_level_long(Plugin::LogEntry::Entry::Level l) {
	return nsclient::logging::logger_helper::render_log_level_short(nscapi::functions::gpb_to_log(l));
}
std::wstring rpad(std::wstring str, int len) {
	if (str.length() > len)
		return str.substr(str.length()-len);
	return std::wstring(len-str.length(), L' ') + str;
}
std::wstring lpad(std::wstring str, int len) {
	if (str.length() > len)
		return str.substr(0, len);
	return str + std::wstring(len-str.length(), L' ');
}
std::wstring render_console_message(const std::string &data) {
	std::wstringstream ss;
	try {
		Plugin::LogEntry message;
		if (!message.ParseFromString(data)) {
			log_fatal("Failed to parse message: " + strEx::strip_hex(data));
			return ss.str();
		}

		for (int i=0;i<message.entry_size();i++) {
			Plugin::LogEntry::Entry msg = message.entry(i);
			if (i > 0)
				ss << _T(" -- ");
			ss << render_log_level_short(msg.level())
				<< _T(" ") << rpad(utf8::cvt<std::wstring>(msg.file()), 40)
				<< _T(":") << lpad(utf8::cvt<std::wstring>(strEx::itos(msg.line())),4) 
				<< _T(" ") + utf8::cvt<std::wstring>(msg.message());
		}
		return ss.str();
	} catch (std::exception &e) {
		log_fatal("Failed to parse data from: " + strEx::strip_hex(data) + ": " + e.what());
	} catch (...) {
		log_fatal("Failed to parse data from: " + strEx::strip_hex(data));
	}
	return ss.str();
}

namespace sh = nscapi::settings_helper;

class simple_file_logger : public nsclient::logging::logging_interface_impl {
	std::string file_;
	int max_size_;
	std::string format_;

public:
	simple_file_logger(std::string file) : max_size_(0), format_("%Y-%m-%d %H:%M:%S") {
		file_ = base_path() + file;
	}
	std::string base_path() {
		unsigned int buf_len = 4096;
		char* buffer = new char[buf_len+1];
		GetModuleFileNameA(NULL, buffer, buf_len);
		std::string path = buffer;
		std::string::size_type pos = path.rfind('\\');
		path = path.substr(0, pos+1);
		delete [] buffer;
		return path;
	}


	void do_log(const std::string &data) {
		if (file_.empty())
			return;
		try {
			if (max_size_ != 0 &&  boost::filesystem::exists(file_.c_str()) && boost::filesystem::file_size(file_.c_str()) > max_size_) {
				int target_size = max_size_*0.7;
				char *tmpBuffer = new char[target_size+1];
				try {
					std::ifstream ifs(file_.c_str());
					ifs.seekg(-target_size, std::ios_base::end);
					ifs.read(tmpBuffer, target_size);
					ifs.close();
					std::ofstream ofs(file_.c_str(), std::ios::trunc);
					ofs.write(tmpBuffer, target_size);
				} catch (...) {
					log_fatal("Failed to truncate log file: " + file_);
				}
				delete [] tmpBuffer;
			}
			std::ofstream stream(file_.c_str(), std::ios::out|std::ios::app|std::ios::ate);
			if (!stream) {
				log_fatal("File could not be opened, Discarding: " + strEx::strip_hex(data));
			}
			std::string date = nsclient::logging::logger_helper::get_formated_date(format_);

			Plugin::LogEntry message;
			if (!message.ParseFromString(data)) {
				log_fatal("Failed to parse message: " + strEx::strip_hex(data));
			} else {
				for (int i=0;i<message.entry_size();i++) {
					Plugin::LogEntry::Entry msg = message.entry(i);
					stream << date
						<< (": ") << utf8::cvt<std::string>(render_log_level_long(msg.level()))
						<< (":") << msg.file()
						<< (":") << msg.line()
						<< (": ") << msg.message() << std::endl;
				}
			}
		} catch (std::exception &e) {
			log_fatal("Failed to parse data from: " + strEx::strip_hex(data) + ": " + e.what());
		} catch (...) {
			log_fatal("Failed to parse data from: " + strEx::strip_hex(data));
		}
	}
	void configure() {
		try {
			std::wstring file;

			sh::settings_registry settings(settings_manager::get_proxy());
			settings.set_alias(_T("log/file"));

			settings.add_path_to_settings()
				(_T("log"),_T("LOG SECTION"), _T("Configure log properties."))

				(_T("log/file"), _T("LOG SECTION"), _T("Configure log file properties."))
				;


			settings.add_key_to_settings(_T("log"))
				(_T("file name"), sh::wstring_key(&file, _T("${exe-path}/nsclient.log")),
				_T("FILENAME"), _T("The file to write log data to. Set this to none to disable log to file."))

				(_T("date format"), sh::string_key(&format_, "%Y-%m-%d %H:%M:%S"),
				_T("DATEMASK"), _T("The size of the buffer to use when getting messages this affects the speed and maximum size of messages you can recieve."))

				;

			settings.add_key_to_settings(_T("log/file"))
				(_T("max size"), sh::int_key(&max_size_, 0),
				_T("MAXIMUM FILE SIZE"), _T("When file size reaches this it will be truncated to 50% if set to 0 (default) truncation will be disabled"))
				;

			settings.register_all();
			settings.notify();

			file_ = utf8::cvt<std::string>(settings_manager::get_proxy()->expand_path(file));
			if (file_.empty())
				file_ = base_path() + "nsclient.log";
			if (file_.find('\\') == std::string::npos && file_.find('/') == std::string::npos) {
				file_ = base_path() + file_;
			}
			if (file_ == "none") {
				file_ = "";
			}

		} catch (nscapi::nscapi_exception &e) {
			log_fatal(std::string("Failed to register command: ") + e.what());
		} catch (std::exception &e) {
			log_fatal(std::string("Exception caught: ") + e.what());
		} catch (...) {
			log_fatal("Failed to register command.");
		}
	}
	bool startup() { return true; }
	bool shutdown() { return true; }
};





const static std::string QUIT_MESSAGE = "$$QUIT$$";
const static std::string CONFIGURE_MESSAGE = "$$CONFIGURE$$";

typedef boost::shared_ptr<nsclient::logging::logging_interface_impl> log_impl_type;


class threaded_logger : public nsclient::logging::logging_interface_impl {
	concurrent_queue<std::string> log_queue_;
	boost::thread thread_;

	log_impl_type background_logger_;

public:

	threaded_logger(log_impl_type background_logger) : background_logger_(background_logger) {}

	void do_log(const std::string &data) {
		if (get_console_log()) {
			std::wcout << render_console_message(data) << std::endl;
		}
		push(data);
	}
	void push(const std::string &data) {
		log_queue_.push(data);
	}

	void thread_proc() {
		std::string data;
		while (true) {
			log_queue_.wait_and_pop(data);
			if (data == QUIT_MESSAGE) {
				break;
			} else if (data == CONFIGURE_MESSAGE) {
				if (background_logger_)
					background_logger_->configure();
			} else {
				if (background_logger_)
					background_logger_->do_log(data);
				subscribers_.notify(data);
			}
		}
	}

	void configure() {
		push(CONFIGURE_MESSAGE);
	}
	bool startup() {
		thread_ = boost::thread(boost::bind(&threaded_logger::thread_proc, this));
		return true;
	}
	bool shutdown() {
		push(QUIT_MESSAGE);
		if (!thread_.timed_join(boost::posix_time::seconds(5))) {
			log_fatal("Failed to exit log slave!");
			return false;
		}
		return true;
	}

	virtual void set_log_level(NSCAPI::log_level::level level) {
		nsclient::logging::logging_interface_impl::set_log_level(level);
		background_logger_->set_log_level(level);
	}
	virtual void set_console_log(bool console_log) {
		nsclient::logging::logging_interface_impl::set_console_log(console_log);
		background_logger_->set_console_log(console_log);
	}
};

static nsclient::logging::logging_interface_impl *impl = NULL;

nsclient::logging::logging_interface_impl* get_impl() {
	if (impl == NULL)
		impl = new threaded_logger(log_impl_type(new simple_file_logger("nsclient.log")));
	return impl;
}


nsclient::logging::logger_interface* nsclient::logging::logger::get_logger() {
	return get_impl();
}

void nsclient::logging::logger::subscribe_raw(raw_subscriber_type subscriber) {
	subscribers_.add(subscriber);
}
void nsclient::logging::logger::clear_subscribers() {
	subscribers_.clear();
}
bool nsclient::logging::logger::startup() {
	return get_impl()->startup();
}
bool nsclient::logging::logger::shutdown() {
	return get_impl()->shutdown();
}
void nsclient::logging::logger::configure() {
	return get_impl()->configure();
}

void nsclient::logging::logger::set_log_level(NSCAPI::log_level::level level) {
	get_impl()->set_log_level(level);
}
void nsclient::logging::logger::set_log_level(std::wstring level) {
	get_impl()->set_log_level(nscapi::logging::parse(level));
}
