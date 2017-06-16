/// \file  NetOut.cpp
/// \brief Caliper text log service

#include "../CaliperService.h"

#include <curl/curl.h>

#include "caliper/Caliper.h"
#include "caliper/SnapshotRecord.h"

#include "caliper/common/SnapshotTextFormatter.h"
#include "caliper/common/Log.h"
#include "caliper/common/RuntimeConfig.h"

#include "caliper/common/util/split.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

using namespace cali;
using namespace std;

namespace
{

const ConfigSet::Entry   configdata[] = {
    { "trigger", CALI_TYPE_STRING, "",
      "List of attributes for which to write text log entries",
      "Colon-separated list of attributes for which to write text log entries."
    },
    { "formatstring", CALI_TYPE_STRING, "",
      "Format of the text log output",
      "Description of the text log format output. If empty, a default one will be created."
    },
    { "filename", CALI_TYPE_STRING, "stdout",
      "File name for event record stream. Auto-generated by default.",
      "File name for event record stream. Either one of\n"
      "   stdout: Standard output stream,\n"
      "   stderr: Standard error stream,\n"
      "   none:   No output,\n"
      " or a file name. The default is stdout\n"
    },
    { "posturl" , CALI_TYPE_STRING, "https://lc.llnl.gov",
      "URL to issue requests to"
    },
    ConfigSet::Terminator
};

class NetOutService
{
    ConfigSet                   config;

    std::mutex                  trigger_attr_mutex;
    typedef std::map<cali_id_t, Attribute> TriggerAttributeMap;
    TriggerAttributeMap         trigger_attr_map;

    CURL*                        m_curl;

    std::vector<std::string>    trigger_attr_names;

    SnapshotTextFormatter       formatter;
    std::stringstream           string_output;
    enum class Stream { None, File, StdErr, StdOut };

    Stream                      m_stream;
    ofstream                    m_ofstream;
    std::string                 m_output_url;
    Attribute                   set_event_attr;   
    Attribute                   end_event_attr;

    static unique_ptr<NetOutService> 
                                s_netout;

    std::string 
    create_default_formatstring(const std::vector<std::string>& attr_names) {
        if (attr_names.size() < 1)
            return "%time.inclusive.duration%";

        int name_sizes = 0;

        for (const std::string& s : attr_names)
            name_sizes += s.size();

        int w = max<int>(0, (80-10-name_sizes-2*attr_names.size()) / attr_names.size());

        std::ostringstream os;

        for (const std::string& s : attr_names)
            os << s << "=%[" << w << "]" << s << "% ";

        os << "%[8r]time.inclusive.duration%";

        return os.str();
    }

    void init_stream() {
        string filename = config.get("filename").to_string();

        const map<string, Stream> strmap { 
            { "none",   Stream::None   },
            { "stdout", Stream::StdOut },
            { "stderr", Stream::StdErr } };

        auto it = strmap.find(filename);

        if (it == strmap.end()) {
            m_ofstream.open(filename);

            if (!m_ofstream)
                Log(0).stream() << "Could not open text log file " << filename << endl;
            else
                m_stream = Stream::File;
        } else
            m_stream = it->second;
    }

    std::ostream& get_stream() {
        switch (m_stream) {
        case Stream::StdOut:
            return std::cout;
        case Stream::StdErr:
            return std::cerr;
        default:
            return m_ofstream;
        }
    }

    void create_attribute_cb(Caliper* c, const Attribute& attr) {
        if (attr.skip_events())
            return;

        std::vector<std::string>::iterator it = 
            find(trigger_attr_names.begin(), trigger_attr_names.end(), attr.name());

        if (it != trigger_attr_names.end()) {
            std::lock_guard<std::mutex> lock(trigger_attr_mutex);
            trigger_attr_map.insert(std::make_pair(attr.id(), attr));
        }
    }

    void process_snapshot_cb(Caliper* c, const SnapshotRecord* trigger_info, const SnapshotRecord* snapshot) {
        // operate only on cali.snapshot.event.end attributes for now
        if (!trigger_info)
            return;

        Entry event = trigger_info->get(end_event_attr);

        if (event.is_empty())
            event = trigger_info->get(set_event_attr);
        if (event.is_empty())
            return;

        Attribute trigger_attr { Attribute::invalid };

        {
            std::lock_guard<std::mutex> lock(trigger_attr_mutex);

            TriggerAttributeMap::const_iterator it = 
                trigger_attr_map.find(event.value().to_id());

            if (it != trigger_attr_map.end())
                trigger_attr = it->second;
        }

        if (trigger_attr == Attribute::invalid || snapshot->get(trigger_attr).is_empty())
            return;

        std::vector<Entry> entrylist;

        SnapshotRecord::Sizes size = snapshot->size();
        SnapshotRecord::Data  data = snapshot->data();

        for (size_t n = 0; n < size.n_nodes; ++n)
            entrylist.push_back(Entry(data.node_entries[n]));
        for (size_t n = 0; n < size.n_immediate; ++n)
            entrylist.push_back(Entry(data.immediate_attr[n], data.immediate_data[n]));

        // DZPOLIA EDITING HERE
        formatter.print(string_output, c, entrylist) << std::endl;
        std::string outThis = string_output.str();
        curl_easy_setopt(m_curl,CURLOPT_URL,m_output_url.c_str());
        curl_easy_setopt(m_curl,CURLOPT_USERAGENT,"libcurl-agent/1.0");
        curl_easy_setopt(s_netout->getCurl(),CURLOPT_POSTFIELDS,outThis.c_str());
        curl_easy_setopt(s_netout->getCurl(),CURLOPT_POSTFIELDSIZE,outThis.length());
        CURLcode result = curl_easy_perform(m_curl);
        if(result != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
            curl_easy_strerror(result));
        }
        else{
            fprintf(stderr, "curl_easy_perform() success\n");
        }
        
    }

    void post_init_cb(Caliper* c) {
        std::string formatstr = config.get("formatstring").to_string();
        curl_global_init(CURL_GLOBAL_ALL); 
        m_output_url = config.get("posturl").to_string();
        // DZPOLIA OPTIONS HERE
        m_curl = curl_easy_init();
        if (formatstr.size() == 0)
            formatstr = create_default_formatstring(trigger_attr_names);

        formatter.reset(formatstr);

        set_event_attr      = c->get_attribute("cali.snapshot.event.set");
        end_event_attr      = c->get_attribute("cali.snapshot.event.end");

        if (end_event_attr      == Attribute::invalid ||
            set_event_attr      == Attribute::invalid)
            Log(1).stream() << "NetOut: Note: \"event\" trigger attributes not registered\n"
                "    disabling text log.\n" << std::endl;
    }

    // static callbacks

    static void s_create_attribute_cb(Caliper* c, const Attribute& attr) { 
        s_netout->create_attribute_cb(c, attr);
    }

    static void s_process_snapshot_cb(Caliper* c, const SnapshotRecord* trigger_info, const SnapshotRecord* snapshot) {
        s_netout->process_snapshot_cb(c, trigger_info, snapshot);
    }

    static void s_post_init_cb(Caliper* c) { 
        s_netout->post_init_cb(c);
    }
    decltype(m_curl) getCurl(){
        return m_curl;
    }
    NetOutService(Caliper* c)
        : config(RuntimeConfig::init("netout", configdata)),
          set_event_attr(Attribute::invalid),
          end_event_attr(Attribute::invalid)
        { 
            init_stream();

            util::split(config.get("trigger").to_string(), ':', 
                        std::back_inserter(trigger_attr_names));

            c->events().create_attr_evt.connect(&NetOutService::s_create_attribute_cb);
            c->events().post_init_evt.connect(&NetOutService::s_post_init_cb);
            c->events().process_snapshot.connect(&NetOutService::s_process_snapshot_cb);

            Log(1).stream() << "Registered text log service" << std::endl;
        }

public:
    
    static void netout_register(Caliper* c) {
        s_netout.reset(new NetOutService(c));
    }

}; // NetOutService

unique_ptr<NetOutService> NetOutService::s_netout { nullptr };

} // namespace

namespace cali
{
    CaliperService netout_service = { "netout", ::NetOutService::netout_register };
} // namespace cali
