/*  Copyright (c) MediaArea.net SARL. All Rights Reserved.
 *
 *  Use of this source code is governed by a GPLv3+/MPLv2+ license that can
 *  be found in the License.html file in the root of the source tree.
 */

//---------------------------------------------------------------------------
#ifdef __BORLANDC__
    #pragma hdrstop
#endif
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#include "Core.h"
#include "Scheduler.h"
#include "Queue.h"
#include "Plugin.h"
#include "PluginFormat.h"
#include "PluginsManager.h"
#include "VeraPDF.h"
#include "DpfManager.h"
#include "PluginLog.h"
#include "PluginFileLog.h"
#include "FFmpeg.h"
#include <ZenLib/Ztring.h>

#if defined(_WIN32) || defined(WIN32)
#include <Winbase.h>
#else
#include <sys/time.h>
#endif

//---------------------------------------------------------------------------
namespace MediaConch {

    //***************************************************************************
    // Constructor/Destructor
    //***************************************************************************

    //---------------------------------------------------------------------------
    Scheduler::Scheduler(Core* c) : core(c), max_threads(1)
    {
        queue = new Queue(this);
    }

    //---------------------------------------------------------------------------
    Scheduler::~Scheduler()
    {
        queue->clear();
        CS.Enter();
        std::map<QueueElement*, QueueElement*>::iterator it = working.begin();
        for (; it != working.end(); ++it)
        {
            if (it->first)
            {
                it->first->stop();
                delete it->first;
            }
        }
        working.clear();
        CS.Leave();
        delete queue;
    }

    //---------------------------------------------------------------------------
    int Scheduler::add_element_to_queue(int user, const std::string& filename, long file_id,
                                        const std::vector<std::pair<std::string,std::string> >& options,
                                        const std::vector<std::string>& plugins, bool mil_analyze)
    {
        static int index = 0;

        queue->add_element(PRIORITY_NONE, index++, user, filename, file_id, options, plugins, mil_analyze);
        run_element();
        return index - 1;
    }

    //---------------------------------------------------------------------------
    void Scheduler::run_element()
    {
        CS.Enter();
        if (working.size() >= max_threads)
        {
            CS.Leave();
            return;
        }

        QueueElement *ret = queue->run_next();
        if (!ret)
        {
            CS.Leave();
            return;
        }

        working[ret] = ret;
        CS.Leave();
    }

    void Scheduler::work_finished(QueueElement *el, MediaInfoNameSpace::MediaInfo* MI)
    {
        if (!el)
            return;

        core->set_file_analyzed_to_database(el->user, el->file_id);

        if (!MI)
        {
            CS.Enter();
            remove_element(el);
            CS.Leave();
            run_element();
            return;
        }

        if (another_work_to_do(el, MI) <= 0)
            return;

        CS.Enter();
        core->register_reports_to_database(el->user, el->file_id, MI);
        remove_element(el);
        CS.Leave();
        run_element();
    }

    bool Scheduler::is_finished()
    {
        CS.Enter();
        size_t size = working.size();
        CS.Leave();
        return size == 0;
    }

    void Scheduler::get_elements(int user, std::vector<std::string>& vec)
    {
        CS.Enter();
        std::map<QueueElement*, QueueElement*>::iterator it = working.begin();
        for (; it != working.end(); ++it)
            if (it->first && it->first->user == user)
                vec.push_back(it->first->filename);
        CS.Leave();
    }

    bool Scheduler::element_is_finished(int user, long file_id, double& percent_done)
    {
        bool ret = true;
        CS.Enter();

        std::map<QueueElement*, QueueElement*>::iterator it = working.begin();
        for (; it != working.end(); ++it)
            if (it->first->file_id == file_id && it->first->user == user)
                break;

        if (it != working.end())
        {
            percent_done = it->first->percent_done();
            CS.Leave();
            return false;
        }

        if (queue->has_id(user, file_id) >= 0)
        {
            percent_done = 0.0;
            ret = false;
        }

        CS.Leave();
        return ret;
    }

    long Scheduler::element_exists(int user, const std::string& filename)
    {
        CS.Enter();
        long file_id = queue->has_element(user, filename);
        if (file_id >= 0)
        {
            CS.Leave();
            return file_id;
        }

        std::map<QueueElement*, QueueElement*>::iterator it = working.begin();
        for (; it != working.end(); ++it)
            if (it->first->filename == filename && user == it->first->user)
                break;

        if (it != working.end())
            file_id = it->first->file_id;
        CS.Leave();
        return file_id;
    }

    int Scheduler::another_work_to_do(QueueElement *el, MediaInfoNameSpace::MediaInfo* MI)
    {
        // Before registering, check the format
        String format = MI->Get(Stream_General, 0, __T("Format"));
        std::string format_str = ZenLib::Ztring(format).To_UTF8().c_str();

        std::map<std::string, Plugin*> plugins = core->get_format_plugins();
        if (plugins.find(format_str) == plugins.end() || !plugins[format_str])
            return 1;

        std::string error;
        Plugin *p = NULL;
        if (plugins[format_str]->get_name() == "VeraPDF")
            p = new VeraPDF(*(VeraPDF*)plugins[format_str]);
        else if (plugins[format_str]->get_name() == "DPFManager")
            p = new DPFManager(*(DPFManager*)plugins[format_str]);
        else
            return 1;

        ((PluginFormat*)p)->set_file(el->filename);
        if (p->run(error) < 0)
            core->plugin_add_log(PluginLog::LOG_LEVEL_ERROR, error);
        const std::string& report = p->get_report();
        MediaConchLib::report report_kind = ((PluginFormat*)p)->get_report_kind();

        CS.Enter();
        core->register_reports_to_database(el->user, el->file_id, report, report_kind, MI);
        remove_element(el);
        CS.Leave();
        run_element();

        delete p;

        return 0;
    }

    void Scheduler::remove_element(QueueElement *el)
    {
        std::map<QueueElement*, QueueElement*>::iterator it = working.find(el);
        if (it != working.end())
            working.erase(it);
    }

    int Scheduler::execute_pre_hook_plugins(QueueElement *el, std::string& err, bool& analyze_file)
    {
        // Before registering, check the format
        std::vector<Plugin*> plugins = core->get_pre_hook_plugins();
        std::string          new_file = el->filename;
        long                 old_id = el->file_id;
        int                  ret = 0;

        for (size_t i = 0; i < plugins.size(); ++i)
        {
            if (!plugins[i])
                continue;

            bool is_el_plugin = false;
            for (size_t j = 0; j < el->plugins.size(); ++j)
                if (el->plugins[j] == plugins[i]->get_id())
                    is_el_plugin = true;
            if (!is_el_plugin)
                continue;

            Plugin *p = NULL;
            if (plugins[i]->get_name() == "FFmpeg")
                p = new FFmpeg(*(FFmpeg*)plugins[i]);
            else if (plugins[i]->get_name() == "FileLog")
                p = new PluginFileLog(*(PluginFileLog*)plugins[i]);
            else
                continue;

            ((PluginPreHook*)p)->set_input_file(new_file);

#if defined(_WIN32) || defined(WIN32)
            unsigned long time_before = GetTickCount();
#else
            struct timeval time_before;
            gettimeofday(&time_before, NULL);
#endif

            ret = p->run(err);

#if defined(_WIN32) || defined(WIN32)
            unsigned long time_after = GetTickCount();
            size_t time_passed = time_after - time_before;
#else
            struct timeval time_after;
            gettimeofday(&time_after, NULL);

            size_t time_passed = (time_after.tv_sec - time_before.tv_sec) * 1000 + (time_after.tv_usec - time_before.tv_usec) / 1000;
#endif

            if (ret == 0 && ((PluginPreHook*)p)->is_creating_file())
            {
                std::string generated_log = ((PluginPreHook*)p)->get_report();
                std::string generated_error_log = ((PluginPreHook*)p)->get_report_err();
                new_file = ((PluginPreHook*)p)->get_output_file();

                std::vector<std::pair<std::string,std::string> > options;
                std::map<std::string, std::string>::iterator it = el->options.begin();
                for (; it != el->options.end(); ++it)
                    options.push_back(std::make_pair(it->first, it->second));

                std::vector<std::string> plugins;
                long id = core->checker_analyze(el->user, new_file, old_id, time_passed, generated_log,
                                                generated_error_log, options, plugins, el->mil_analyze);
                if (id >= 0)
                    core->file_update_generated_file(el->user, old_id, id);
                old_id = id;
            }
            else if (ret)
            {
                std::string error_log = ((PluginPreHook*)p)->get_report_err();
                core->update_file_error(el->user, old_id, true, error_log);
                delete p;
                return ret;
            }

            if (!((PluginPreHook*)p)->analyzing_source())
                analyze_file = false;
            delete p;
        }

        if (!analyze_file)
            remove_element(el);

        return ret;
    }

}
