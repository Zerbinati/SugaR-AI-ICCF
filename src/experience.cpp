/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
#include <string.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <stdio.h> //For: remove()
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include "misc.h"
#include "uci.h"
#include "experience.h"

using namespace std;

namespace Experience
{
    typedef unordered_map<Key, ExpEntryEx*> ExpMap;
    typedef unordered_map<Key, ExpEntryEx*>::iterator ExpIterator;
    typedef unordered_map<Key, ExpEntryEx*>::const_iterator ExpConstIterator;

    namespace
    {
        const char *ExperienceSignature = "SugaR";
        const size_t ExperienceSignatureLength = strlen(ExperienceSignature) * sizeof(char);
        
        class ExperienceData
        {
        private:
            string                  _filename;
            vector<ExpEntryEx*>     _expExData;

            ExpMap                  _mainExp;
            vector<ExpEntry*>       _newPvExp;
            vector<ExpEntry*>       _newMultiPvExp;

            bool                    _loading;
            std::atomic<bool>       _abortLoading;      //Only used when destructing
            std::atomic<bool>       _loadingResult;
            std::thread             *_loaderThread;
            std::condition_variable _loadingCond;
            std::mutex              _loaderMutex;

        private:
            void clear()
            {
                //Make sure we are not loading an experience file
                _abortLoading.store(true, std::memory_order_relaxed);
                wait_for_load_finished();
                assert(_loaderThread == nullptr);

                //Free
                for (ExpEntryEx *&p : _expExData)
                    free(p);

                //Clear
                _mainExp.clear();
                _expExData.clear();

                //Clear new exp
                clear_new_exp();
            }

            void clear_new_exp()
            {
                //Delete PV experience
                for (const ExpEntry* exp : _newPvExp)
                    delete exp;

                //Delete NonPV experience
                for (const ExpEntry* exp : _newMultiPvExp)
                    delete exp;
                
                //Clear vectors
                _newPvExp.clear();
                _newMultiPvExp.clear();
            }

            bool link_entry(ExpEntryEx *expEx)
            {
                ExpIterator itr = _mainExp.find(expEx->key);

                //If new entry: insert into map and continue
                if (itr == _mainExp.end())
                {
                    _mainExp[expEx->key] = expEx;
                    return true;
                }

                //If existing entry and same move exists then merge
                ExpEntryEx* expEx2 = itr->second->find(expEx->move);
                if (expEx2)
                {
                    expEx2->merge(expEx);
                    return false;
                }

                //If existing entry and different move then insert sorted based on depth/value
                expEx2 = itr->second;
                do
                {
                    if (expEx->compare(expEx2) > 0)
                    {
                        if (expEx2 == itr->second)
                        {
                            itr->second = expEx;
                            expEx->next = expEx2;
                        }
                        else
                        {
                            expEx->next = expEx2->next;
                            expEx2->next = expEx;
                        }

                        return true;
                    }

                    if (!expEx2->next)
                    {
                        expEx2->next = expEx;
                        return true;
                    }

                    expEx2 = expEx2->next;
                } while (true);

                //Should never reach here!
                assert(false);
                return true;
            }

            bool _load(string fn)
            {
                ifstream in(Utility::map_path(fn), ios::in | ios::binary | ios::ate);
                if (!in.is_open())
                {
                    sync_cout << "info string Could not open experience file: " << fn << sync_endl;
                    return false;
                }

                size_t inSize = in.tellg();
                if (inSize == 0)
                {
                    sync_cout << "info string The experience file [" << fn << "] is empty" << sync_endl;
                    return false;
                }

                size_t expDataSize = inSize - ExperienceSignatureLength;
                size_t expCount = expDataSize / sizeof(ExpEntry);
                if (expCount * sizeof(ExpEntry) != expDataSize)
                {
                    sync_cout << "info string Experience file [" << fn << "] is corrupted. Size: " << inSize << ", exp-size: " << expDataSize << ", exp-count: " << expCount << sync_endl;
                    return false;
                }

                //Seek to beginning of file
                in.seekg(ios::beg);

                //Check signature
                char* sig = (char*)malloc(ExperienceSignatureLength);
                if (!sig)
                {
                    sync_cout << "info string Failed to allocate " << ExperienceSignatureLength << " bytes for experience signature verification" << sync_endl;
                    return false;
                }

                if (!in.read(sig, ExperienceSignatureLength))
                {
                    sync_cout << "info string Failed to read " << ExperienceSignatureLength << " bytes for experience signature verification" << sync_endl;
                    return false;
                }

                if (memcmp(sig, ExperienceSignature, ExperienceSignatureLength) != 0)
                {
                    free(sig);

                    sync_cout << "info string Experience file [" << fn << "] signature missmatch " << sync_endl;
                    return false;
                }

                //Free signature memory
                free(sig);

                //Allocate buffer for ExpEx data
                ExpEntryEx* expData = (ExpEntryEx*)malloc(expCount * sizeof(ExpEntryEx));
                if (!expData)
                {
                    sync_cout << "info string Failed to allocate " << expCount * sizeof(ExpEntryEx) << " bytes for stored experience data from file [" << fn << "]" << sync_endl;
                    return false;
                }

                //Few variables to be used for statistical information
                size_t prevPosCount = _mainExp.size();

                //Load experience entries
                size_t duplicateMoves = 0;
                for (size_t i = 0; i < expCount; i++)
                {
                    if (_abortLoading.load(std::memory_order_relaxed))
                        break;

                    //Prepare to read
                    ExpEntryEx* expEx = expData + i;
                    expEx->next = nullptr;

                    //Read
                    if (!in.read((char*)expEx, sizeof(ExpEntry)))
                    {
                        free(expData);

                        sync_cout << "info string Failed to read " << sizeof(ExpEntryEx) << " bytes of experience entry " << i + 1 << " of " << expCount << sync_endl;
                        return false;
                    }

                    //Merge
                    if (!link_entry(expEx))
                        duplicateMoves++;
                }

                //Add buffer to vector so that it will be released later
                _expExData.push_back(expData);

                //Nothing to do if loading was aborted
                if (_abortLoading.load(std::memory_order_relaxed))
                    return false;

                //Show some statistics
                if (prevPosCount)
                {
                    sync_cout
                        << "info string " << fn << " -> Total new moves: " << expCount
                        << ". Total new positions: " << (_mainExp.size() - prevPosCount)
                        << ". Duplicate moves: " << duplicateMoves
                        << sync_endl;
                }
                else
                {
                    sync_cout
                        << "info string " << fn << " -> Total moves: " << expCount
                        << ". Total positions: " << _mainExp.size()
                        << ". Duplicate moves: " << duplicateMoves
                        << ". Fragmentation: " << std::setprecision(2) << std::fixed << 100.0 * (double)duplicateMoves / (double)expCount << "%"
                        << sync_endl;
                }

                return true;
            }

            bool _save(string fn, bool saveAll)
            {
                fstream out;
                out.open(Utility::map_path(fn), ios::out | ios::binary | ios::app);
                if (!out.is_open())
                {
                    sync_cout << "info string Failed to open experience file [" << fn << "] for writing" << sync_endl;
                    return false;
                }

                //If this is a new file then we need to write the signature first
                out.seekg(0, out.end);
                size_t length = out.tellg();
                out.seekg(0, out.beg);

                if (length == 0)
                {
                    out.seekp(0, out.beg);

                    if (!out.write(ExperienceSignature, ExperienceSignatureLength))
                    {
                        sync_cout << "info string Failed to write signature to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }
                }

                //Reposition writing pointer to end of file
                out.seekp(ios::end);

                size_t allMoves = 0;
                size_t allPositions = 0;
                if (saveAll)
                {
                    const ExpEntryEx* p = nullptr;
                    for (const auto& x : _mainExp)
                    {
                        allPositions++;
                        p = x.second;

                        while (p)
                        {
                            if (p->depth >= MIN_EXP_DEPTH)
                            {
                                allMoves++;
                                out.write((const char*)p, sizeof(ExpEntry));
                            }

                            p = p->next;
                        }
                    }
                }

                //Save new PV experience
                int newPvExpCount = 0;
                for (const ExpEntry* e : _newPvExp)
                {
                    if (!e)
                        continue;

                    if (e->depth < MIN_EXP_DEPTH)
                        continue;

                    out.write((const char*)e, sizeof(ExpEntry));
                    if (!out)
                    {
                        sync_cout << "info string Failed to save new PV experience entry to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }

                    newPvExpCount++;
                }

                //Save new MultiPV experience
                int newMultiPvExpCount = 0;
                for (const ExpEntry* e : _newMultiPvExp)
                {
                    if (!e)
                        continue;

                    if (e->depth < MIN_EXP_DEPTH)
                        continue;

                    out.write((const char*)e, sizeof(ExpEntry));

                    if (!out)
                    {
                        sync_cout << "info string Failed to save new MultiPV experience entry to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }

                    newMultiPvExpCount++;
                }

                //Clear new moves
                clear_new_exp();

                if (saveAll)
                {
                    sync_cout << "info string Saved " << allPositions << " position(s) and " << allMoves << " moves to experience file: " << fn << sync_endl;
                }
                else
                {
                    sync_cout << "info string Saved " << newPvExpCount << " PV and " << newMultiPvExpCount << " MultiPV entries to experience file: " << fn << sync_endl;
                }

                return true;
            }

        public:
            ExperienceData()
            {
                _loading = false;
                _abortLoading.store(false, std::memory_order_relaxed);
                _loadingResult.store(false, std::memory_order_relaxed);
                _loaderThread = nullptr;
            }

            ~ExperienceData()
            {
                clear();
            }

            string filename() const
            {
                return _filename;
            }

            bool has_new_exp() const
            {
                return _newPvExp.size() || _newMultiPvExp.size();
            }

            bool load(string filename, bool synchronous)
            {
                //Make sure we are not already in the process of loading same/other experience file
                wait_for_load_finished();

                //Load requested experience file
                _filename = filename;
                _loadingResult.store(false, std::memory_order_relaxed);

                //Block
                {
                    _loading = true;
                    std::lock_guard<std::mutex> lg1(_loaderMutex);
                    _loaderThread = new std::thread(std::thread([this, filename]()
                        {
                            //Load
                            bool loadingResult = _load(filename);
                            _loadingResult.store(loadingResult, std::memory_order_relaxed);

                            //Notify
                            {
                                std::lock_guard<std::mutex> lg2(_loaderMutex);
                                _loading = false;
                                _loadingCond.notify_one();
                            }
                            
                            //Detach and delete thread
                            _loaderThread->detach();
                            delete _loaderThread;
                            _loaderThread = nullptr;
                        }));
                }

                return synchronous ? wait_for_load_finished() : true;
            }

            bool wait_for_load_finished()
            {
                std::unique_lock<std::mutex> ul(_loaderMutex);
                _loadingCond.wait(ul, [&] { return !_loading; });
                return loading_result();
            }

            bool loading_result() const
            {
                return _loadingResult.load(std::memory_order_relaxed);
            }

            void save(string fn, bool saveAll)
            {
                //Make sure we are not already in the process of loading same/other experience file
                wait_for_load_finished();

                if (!has_new_exp() && (!saveAll || _mainExp.size() == 0))
                    return;

                //Step 1: Create backup only if 'saveAll' is 'true'
                string expFilename = Utility::map_path(fn);
                string backupExpFilename;
                if (saveAll && Utility::file_exists(expFilename))
                {
                    backupExpFilename = expFilename + ".bak";

                    //If backup file already exists then delete it
                    if (Utility::file_exists(backupExpFilename))
                    {
                        if (remove(backupExpFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not deleted existing backup file: " << backupExpFilename << sync_endl;

                            //Clear backup filename
                            backupExpFilename.clear();
                        }
                    }

                    //Rename current experience file
                    if (!backupExpFilename.empty())
                    {
                        if (rename(expFilename.c_str(), backupExpFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not create backup of current experience file" << sync_endl;

                            //Clear backup filename
                            backupExpFilename.clear();
                        }
                    }
                }

                //Step 2: Save
                if (!_save(fn, saveAll))
                {
                    //Step 2a: Restore backup in case of failure while saving
                    if (!backupExpFilename.empty())
                    {
                        if (rename(backupExpFilename.c_str(), expFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not restore backup experience file: " << backupExpFilename << sync_endl;
                        }
                    }
                }
            }

            const ExpEntryEx* probe(Key k)
            {
                ExpConstIterator itr = _mainExp.find(k);
                if (itr == _mainExp.end())
                    return nullptr;

                assert(itr->second->key == k);

                return itr->second;
            }

            void add_pv_experience(Key k, Move m, Value v, Depth d)
            {
                _newPvExp.emplace_back(new ExpEntry(k, m, v, d));
            }

            void add_multipv_experience(Key k, Move m, Value v, Depth d)
            {
                _newMultiPvExp.emplace_back(new ExpEntry(k, m, v, d));
            }
        };

        ExperienceData*currentExperience = nullptr;
        bool experienceEnabled = true;
        bool learningPaused = false;
    }

    void init()
    {
        experienceEnabled = Options["Experience Enabled"];
        if (!experienceEnabled)
        {
            unload();
            return;
        }

        string filename = Options["Experience File"];
        if (currentExperience)
        {
            if (currentExperience->filename() == filename && currentExperience->loading_result())
                return;

            if (currentExperience)
                unload();
        }

        currentExperience = new ExperienceData();
        currentExperience->load(filename, false);
    }

    bool enabled()
    {
        return experienceEnabled;
    }

    void unload()
    {
        save();

        delete currentExperience;
        currentExperience = nullptr;
    }

    void save()
    {
        if (!currentExperience || !currentExperience->has_new_exp() || (bool)Options["Experience Readonly"])
            return;

        currentExperience->save(currentExperience->filename(), false);
    }

    void reload()
    {
        if (!currentExperience || !currentExperience->has_new_exp())
            return;

        init();
    }

    const ExpEntryEx* probe(Key k)
    {
        assert(experienceEnabled);
        if (!currentExperience)
            return nullptr;

        return currentExperience->probe(k);
    }

    void wait_for_loading_finished()
    {
        if (!currentExperience)
            return;

        currentExperience->wait_for_load_finished();
    }

    //Defrag command:
    //Format:  defrag [filename]
    //Example: defrag C:\Path to\Experience\file.exp
    //Note:    'filename' is optional. If omitted, then the default experience filename (SugaR.exp) will be used
    //         'filename' can contain spaces and can be a full path. If filename contains spaces, it is best to enclose it in quotations
    void defrag(int argc, char* argv[])
    {
        if (argc != 3)
        {
            sync_cout << "info string Error : Incorrect defrag command" << sync_endl;
            sync_cout << "info string Syntax: defrag [filename]" << sync_endl;
            return;
        }

        string filename = Utility::map_path(Utility::unquote(argv[2]));

        //Print message
        sync_cout << "\nDefragmenting experience file: " << filename << sync_endl;

        //Map filename
        filename = Utility::map_path(filename);

        //Load
        ExperienceData exp;
        if (!exp.load(filename, true))
            return;

        //Save
        exp.save(filename, true);
    }

    //Merge command:
    //Format:  merge filename filename1 filename2 ... filenameX
    //Example: defrag "C:\Path to\Experience\file.exp"
    //Note:    'filename' is the target filename, which will also merged with the rest of the files if it exists
    //         'filename1' ... 'filenameX' are the names of the experience files to be merged (along with filename)
    //         'filename' can contain spaces but in that case it needs to eb quoted. It can also be a full path
    void merge(int argc, char* argv[])
    {
        //Step 1: Check
        if (argc < 4)
        {
            sync_cout << "info string Error : Incorrect merge command" << sync_endl;
            sync_cout << "info string Syntax: merge <filename> <filename1> [filename2] ... [filenameX]" << sync_endl;
            sync_cout << "info string The first <filename> is also the target experience file which will contain all the merged data" << sync_endl;
            sync_cout << "info string The files <filename1> ... <filenameX> are the other experience files to be merged" << sync_endl;
            return;
        }

        //Step 2: Collect filenames
        vector<string> filenames;
        for (int i = 2; i < argc; ++i)
            filenames.push_back(Utility::map_path(Utility::unquote(argv[i])));

        //Step 3: The first filename is also the target filename
        string targetFilename = filenames.front();

        //Print message
        sync_cout << "\nMerging experience files: ";
        for (const string& fn : filenames)
            cout << "\n\t" << fn;

        cout << "\nTarget file: " << targetFilename << "\n" << sync_endl;

        //Step 4: Load and merge
        ExperienceData exp;
        for (const string& fn : filenames)
            exp.load(fn, true);

        exp.save(targetFilename, true);
    }

    void pause_learning()
    {
        learningPaused = true;
    }

    void resume_learning()
    {
        learningPaused = false;
    }

    bool is_learning_paused()
    {
        return learningPaused;
    }

    void add_pv_experience(Key k, Move m, Value v, Depth d)
    {
        if (!currentExperience)
            return;

        assert((bool)Options["Experience Readonly"] == false);

        currentExperience->add_pv_experience(k, m, v, d);
    }

    void add_multipv_experience(Key k, Move m, Value v, Depth d)
    {
        if (!currentExperience)
            return;

        assert((bool)Options["Experience Readonly"] == false);

        currentExperience->add_multipv_experience(k, m, v, d);
    }
}

