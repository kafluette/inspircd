/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  Inspire is copyright (C) 2002-2004 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "inspircd_io.h"
#include "inspircd_util.h"
#include "inspircd_config.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <time.h>
#include <string>
#ifdef GCC3
#include <ext/hash_map>
#else
#include <hash_map>
#endif
#include <sstream>
#include <vector>
#include <deque>
#include <stdarg.h>
#include "connection.h"
#include "users.h"
#include "servers.h"
#include "ctables.h"
#include "globals.h"
#include "modules.h"
#include "dynamic.h"
#include "wildcard.h"
#include "message.h"
#include "mode.h"
#include "xline.h"
#include "commands.h"
#include "inspstring.h"
#include "helperfuncs.h"
#include "hashcomp.h"

using namespace std;

extern int MODCOUNT;
extern std::vector<Module*, __single_client_alloc> modules;

extern time_t TIME;
extern bool nofork;
extern char lowermap[255];
extern char ServerName[MAXBUF];
extern char Network[MAXBUF];
extern char ServerDesc[MAXBUF];
extern char list[MAXBUF];

extern int debugging;
extern int LogLevel;

extern std::stringstream config_f;

extern serverrec* me[32];

extern FILE *log_file;
extern userrec* fd_ref_table[65536];

extern int statsAccept, statsRefused, statsUnknown, statsCollisions, statsDns, statsDnsGood, statsDnsBad, statsConnects, statsSent, statsRecv;

static char already_sent[65536];
extern std::vector<userrec*, __single_client_alloc> all_opers;

extern ClassVector Classes;

typedef nspace::hash_map<std::string, userrec*, nspace::hash<string>, irc::StrHashComp, __single_client_alloc> user_hash;
typedef nspace::hash_map<std::string, chanrec*, nspace::hash<string>, irc::StrHashComp, __single_client_alloc> chan_hash;
typedef std::deque<command_t, __single_client_alloc> command_table;

extern user_hash clientlist;
extern chan_hash chanlist;
extern command_table cmdlist;
extern file_cache MOTD;
extern file_cache RULES;

void log(int level,char *text, ...)
{
        char textbuffer[MAXBUF];
        va_list argsPtr;
        time_t rawtime;
        struct tm * timeinfo;
        if (level < LogLevel)
                return;

        time(&rawtime);
        timeinfo = localtime (&rawtime);

        if (log_file)
        {
                char b[MAXBUF];
                va_start (argsPtr, text);
                vsnprintf(textbuffer, MAXBUF, text, argsPtr);
                va_end(argsPtr);
                strlcpy(b,asctime(timeinfo),MAXBUF);
                b[24] = ':';    // we know this is the end of the time string
                fprintf(log_file,"%s %s\n",b,textbuffer);
                if (nofork)
                {
                        // nofork enabled? display it on terminal too
                        printf("%s %s\n",b,textbuffer);
                }
        }
}

void readfile(file_cache &F, const char* fname)
{
        FILE* file;
        char linebuf[MAXBUF];

        log(DEBUG,"readfile: loading %s",fname);
        F.clear();
        file =  fopen(fname,"r");
        if (file)
        {
                while (!feof(file))
                {
                        fgets(linebuf,sizeof(linebuf),file);
                        linebuf[strlen(linebuf)-1]='\0';
                        if (linebuf[0] == 0)
                        {
                                strcpy(linebuf,"  ");
                        }
                        if (!feof(file))
                        {
                                F.push_back(linebuf);
                        }
                }
                fclose(file);
        }
        else
        {
                log(DEBUG,"readfile: failed to load file: %s",fname);
        }
        log(DEBUG,"readfile: loaded %s, %lu lines",fname,(unsigned long)F.size());
}

void Write(int sock,char *text, ...)
{
        if (sock < 0)
                return;
        if (!text)
        {
                log(DEFAULT,"*** BUG *** Write was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF];
        va_list argsPtr;
        char tb[MAXBUF];
        int res;

        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);
        int bytes = snprintf(tb,MAXBUF,"%s\r\n",textbuffer);
        chop(tb);
        if (fd_ref_table[sock])
        {
                int MOD_RESULT = 0;
                FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes));
                fd_ref_table[sock]->AddWriteBuf(tb);
                statsSent += bytes;
        }
        else log(DEFAULT,"ERROR! attempted write to a user with no fd_ref_table entry!!!");
}

/* write a server formatted numeric response to a single socket */

void WriteServ(int sock, char* text, ...)
{
        if (sock < 0)
                return;
        if (!text)
        {
                log(DEFAULT,"*** BUG *** WriteServ was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF],tb[MAXBUF];
        int res;
        va_list argsPtr;
        va_start (argsPtr, text);

        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);
        int bytes = snprintf(tb,MAXBUF,":%s %s\r\n",ServerName,textbuffer);
        chop(tb);
        if (fd_ref_table[sock])
        {
                int MOD_RESULT = 0;
                FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes));
                fd_ref_table[sock]->AddWriteBuf(tb);
                statsSent += bytes;
        }
        else log(DEFAULT,"ERROR! attempted write to a user with no fd_ref_table entry!!!");
}

/* write text from an originating user to originating user */

void WriteFrom(int sock, userrec *user,char* text, ...)
{
        if (sock < 0)
                return;
        if ((!text) || (!user))
        {
                log(DEFAULT,"*** BUG *** WriteFrom was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF],tb[MAXBUF];
        va_list argsPtr;
        int res;
        va_start (argsPtr, text);

        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);
        int bytes = snprintf(tb,MAXBUF,":%s!%s@%s %s\r\n",user->nick,user->ident,user->dhost,textbuffer);
        chop(tb);
        if (fd_ref_table[sock])
        {
                int MOD_RESULT = 0;
                FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes));
                fd_ref_table[sock]->AddWriteBuf(tb);
                statsSent += bytes;
        }
        else log(DEFAULT,"ERROR! attempted write to a user with no fd_ref_table entry!!!");
}

/* write text to an destination user from a source user (e.g. user privmsg) */

void WriteTo(userrec *source, userrec *dest,char *data, ...)
{
        if ((!dest) || (!data))
        {
                log(DEFAULT,"*** BUG *** WriteTo was given an invalid parameter");
                return;
        }
        if (dest->fd == FD_MAGIC_NUMBER)
                return;
        char textbuffer[MAXBUF],tb[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, data);
        vsnprintf(textbuffer, MAXBUF, data, argsPtr);
        va_end(argsPtr);
        chop(tb);

        // if no source given send it from the server.
        if (!source)
        {
                WriteServ(dest->fd,":%s %s",ServerName,textbuffer);
        }
        else
        {
                WriteFrom(dest->fd,source,"%s",textbuffer);
        }
}

/* write formatted text from a source user to all users on a channel
 * including the sender (NOT for privmsg, notice etc!) */

void WriteChannel(chanrec* Ptr, userrec* user, char* text, ...)
{
        if ((!Ptr) || (!user) || (!text))
        {
                log(DEFAULT,"*** BUG *** WriteChannel was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if (otheruser->fd != FD_MAGIC_NUMBER)
                        WriteTo(user,otheruser,"%s",textbuffer);
        }
}

/* write formatted text from a source user to all users on a channel
 * including the sender (NOT for privmsg, notice etc!) doesnt send to
 * users on remote servers */

void WriteChannelLocal(chanrec* Ptr, userrec* user, char* text, ...)
{
        if ((!Ptr) || (!text))
        {
                log(DEFAULT,"*** BUG *** WriteChannel was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if ((otheruser->fd != FD_MAGIC_NUMBER) && (otheruser->fd != -1) && (otheruser != user))
                {
                        if (!user)
                        {
                                WriteServ(otheruser->fd,"%s",textbuffer);
                        }
                        else
                        {
                                WriteTo(user,otheruser,"%s",textbuffer);
                        }
                }
        }
}

void WriteChannelWithServ(char* ServName, chanrec* Ptr, char* text, ...)
{
        if ((!Ptr) || (!text))
        {
                log(DEFAULT,"*** BUG *** WriteChannelWithServ was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);


        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if (otheruser->fd != FD_MAGIC_NUMBER)
                        WriteServ(otheruser->fd,"%s",textbuffer);
        }
}

/* write formatted text from a source user to all users on a channel except
 * for the sender (for privmsg etc) */

void ChanExceptSender(chanrec* Ptr, userrec* user, char* text, ...)
{
        if ((!Ptr) || (!user) || (!text))
        {
                log(DEFAULT,"*** BUG *** ChanExceptSender was given an invalid parameter");
                return;
        }
        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if ((otheruser->fd != FD_MAGIC_NUMBER) && (user != otheruser))
                        WriteFrom(otheruser->fd,user,"%s",textbuffer);
        }
}

std::string GetServerDescription(char* servername)
{
        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),servername))
                                {
                                        return me[j]->connectors[k].GetDescription();
                                }
                        }
                }
                return ServerDesc; // not a remote server that can be found, it must be me.
        }
}

/* write a formatted string to all users who share at least one common
 * channel, including the source user e.g. for use in NICK */

void WriteCommon(userrec *u, char* text, ...)
{
        if (!u)
        {
                log(DEFAULT,"*** BUG *** WriteCommon was given an invalid parameter");
                return;
        }

        if (u->registered != 7) {
                log(DEFAULT,"*** BUG *** WriteCommon on an unregistered user");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        // FIX: Stops a message going to the same person more than once
        bzero(&already_sent,65536);

        bool sent_to_at_least_one = false;

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* otheruser = (userrec*)o;
                                if ((otheruser->fd > 0) && (!already_sent[otheruser->fd]))
                                {
                                        already_sent[otheruser->fd] = 1;
                                        WriteFrom(otheruser->fd,u,"%s",textbuffer);
                                        sent_to_at_least_one = true;
                                }
                        }
                }
        }
        // if the user was not in any channels, no users will receive the text. Make sure the user
        // receives their OWN message for WriteCommon
        if (!sent_to_at_least_one)
        {
                WriteFrom(u->fd,u,"%s",textbuffer);
        }
}

/* write a formatted string to all users who share at least one common
 * channel, NOT including the source user e.g. for use in QUIT */

void WriteCommonExcept(userrec *u, char* text, ...)
{
        if (!u)
        {
                log(DEFAULT,"*** BUG *** WriteCommon was given an invalid parameter");
                return;
        }

        if (u->registered != 7) {
                log(DEFAULT,"*** BUG *** WriteCommon on an unregistered user");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        bzero(&already_sent,65536);

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* otheruser = (userrec*)o;
                                if (u != otheruser)
                                {
                                        if ((otheruser->fd > 0) && (!already_sent[otheruser->fd]))
                                        {
                                                already_sent[otheruser->fd] = 1;
                                                WriteFrom(otheruser->fd,u,"%s",textbuffer);
                                        }
                                }
                        }
                }
        }
}

void WriteOpers(char* text, ...)
{
        if (!text)
        {
                log(DEFAULT,"*** BUG *** WriteOpers was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (std::vector<userrec*>::iterator i = all_opers.begin(); i != all_opers.end(); i++)
        {
                userrec* a = *i;
                if ((a) && (a->fd != FD_MAGIC_NUMBER))
                {
                        if (strchr(a->modes,'s'))
                        {
                                // send server notices to all with +s
                                WriteServ(a->fd,"NOTICE %s :%s",a->nick,textbuffer);
                        }
                }
        }
}

void NoticeAllOpers(userrec *source, bool local_only, char* text, ...)
{
        if ((!text) || (!source))
        {
                log(DEFAULT,"*** BUG *** NoticeAllOpers was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (std::vector<userrec*>::iterator i = all_opers.begin(); i != all_opers.end(); i++)
        {
                userrec* a = *i;
                if ((a) && (a->fd != FD_MAGIC_NUMBER))
                {
                        if (strchr(a->modes,'s'))
                        {
                                // send server notices to all with +s
                                WriteServ(a->fd,"NOTICE %s :*** Notice From %s: %s",a->nick,source->nick,textbuffer);
                        }
                }
        }

        if (!local_only)
        {
                char buffer[MAXBUF];
                snprintf(buffer,MAXBUF,"V %s @* :%s",source->nick,textbuffer);
                NetSendToAll(buffer);
        }
}
// returns TRUE of any users on channel C occupy server 'servername'.

bool ChanAnyOnThisServer(chanrec *c,char* servername)
{
        log(DEBUG,"ChanAnyOnThisServer");

        std::vector<char*> *ulist = c->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* user = (userrec*)o;
                if (!strcasecmp(user->server,servername))
                        return true;
        }
        return false;
}

// returns true if user 'u' shares any common channels with any users on server 'servername'

bool CommonOnThisServer(userrec* u,const char* servername)
{
        log(DEBUG,"ChanAnyOnThisServer");

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* user = (userrec*)o;
                                if (!strcasecmp(user->server,servername))
                                        return true;
                        }
                }
        }
        return false;
}

void NetSendToCommon(userrec* u, char* s)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,"%s %s",CreateSum().c_str(),s);

        log(DEBUG,"NetSendToCommon: '%s' '%s'",u->nick,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (CommonOnThisServer(u,me[j]->connectors[k].GetServerName().c_str()))
                                {
                                        me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                                }
                        }
                }
        }
}


void NetSendToAll(char* s)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,"%s %s",CreateSum().c_str(),s);

        log(DEBUG,"NetSendToAll: '%s'",s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                        }
                }
        }
}


void NetSendToAll_WithSum(char* s,char* u)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,":%s %s",u,s);

        log(DEBUG,"NetSendToAll: '%s'",s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                        }
                }
        }
}

void NetSendToAllAlive(char* s)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,"%s %s",CreateSum().c_str(),s);

        log(DEBUG,"NetSendToAllAlive: '%s'",s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (me[j]->connectors[k].GetState() != STATE_DISCONNECTED)
                                {
                                        me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                                }
                                else
                                {
                                        log(DEBUG,"%s is dead, not sending to it.",me[j]->connectors[k].GetServerName().c_str());
                                }
                        }
                }
        }
}


void NetSendToOne(char* target,char* s)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,"%s %s",CreateSum().c_str(),s);

        log(DEBUG,"NetSendToOne: '%s' '%s'",target,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),target))
                                {
                                        me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                                }
                        }
                }
        }
}

void NetSendToAllExcept(const char* target,char* s)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,"%s %s",CreateSum().c_str(),s);

        log(DEBUG,"NetSendToAllExcept: '%s' '%s'",target,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (strcasecmp(me[j]->connectors[k].GetServerName().c_str(),target))
                                {
                                        me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                                }
                        }
                }
        }
}

void NetSendToAllExcept_WithSum(const char* target,char* s,char* u)
{
        char buffer[MAXBUF];
        snprintf(buffer,MAXBUF,":%s %s",u,s);

        log(DEBUG,"NetSendToAllExcept: '%s' '%s'",target,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (strcasecmp(me[j]->connectors[k].GetServerName().c_str(),target))
                                {
                                        me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
                                }
                        }
                }
        }
}


void WriteMode(const char* modes, int flags, const char* text, ...)
{
        if ((!text) || (!modes) || (!flags))
        {
                log(DEFAULT,"*** BUG *** WriteMode was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);
        int modelen = strlen(modes);

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
                {
                        bool send_to_user = false;

                        if (flags == WM_AND)
                        {
                                send_to_user = true;
                                for (int n = 0; n < modelen; n++)
                                {
                                        if (!hasumode(i->second,modes[n]))
                                        {
                                                send_to_user = false;
                                                break;
                                        }
                                }
                        }
                        else if (flags == WM_OR)
                        {
                                send_to_user = false;
                                for (int n = 0; n < modelen; n++)
                                {
                                        if (hasumode(i->second,modes[n]))
                                        {
                                                send_to_user = true;
                                                break;
                                        }
                                }
                        }

                        if (send_to_user)
                        {
                                WriteServ(i->second->fd,"NOTICE %s :%s",i->second->nick,textbuffer);
                        }
                }
        }
}

void NoticeAll(userrec *source, bool local_only, char* text, ...)
{
        if ((!text) || (!source))
        {
                log(DEFAULT,"*** BUG *** NoticeAll was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
                {
                        WriteFrom(i->second->fd,source,"NOTICE $* :%s",textbuffer);
                }
        }

        if (!local_only)
        {
                char buffer[MAXBUF];
                snprintf(buffer,MAXBUF,"V %s * :%s",source->nick,textbuffer);
                NetSendToAll(buffer);
        }

}


void WriteWallOps(userrec *source, bool local_only, char* text, ...)
{
        if ((!text) || (!source))
        {
                log(DEFAULT,"*** BUG *** WriteOpers was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
                {
                        if (strchr(i->second->modes,'w'))
                        {
                                WriteTo(source,i->second,"WALLOPS :%s",textbuffer);
                        }
                }
        }

        if (!local_only)
        {
                char buffer[MAXBUF];
                snprintf(buffer,MAXBUF,"@ %s :%s",source->nick,textbuffer);
                NetSendToAll(buffer);
        }
}

/* convert a string to lowercase. Note following special circumstances
 * taken from RFC 1459. Many "official" server branches still hold to this
 * rule so i will too;
 *
 *  Because of IRC's scandanavian origin, the characters {}| are
 *  considered to be the lower case equivalents of the characters []\,
 *  respectively. This is a critical issue when determining the
 *  equivalence of two nicknames.
 */

void strlower(char *n)
{
        if (n)
        {
                for (char* t = n; *t; t++)
                        *t = lowermap[*t];
        }
}

/* Find a user record by nickname and return a pointer to it */

userrec* Find(std::string nick)
{
        user_hash::iterator iter = clientlist.find(nick);

        if (iter == clientlist.end())
                /* Couldn't find it */
                return NULL;

        return iter->second;
}

/* find a channel record by channel name and return a pointer to it */

chanrec* FindChan(const char* chan)
{
        if (!chan)
        {
                log(DEFAULT,"*** BUG *** Findchan was given an invalid parameter");
                return NULL;
        }

        chan_hash::iterator iter = chanlist.find(chan);

        if (iter == chanlist.end())
                /* Couldn't find it */
                return NULL;

        return iter->second;
}


long GetMaxBans(char* name)
{
        char CM[MAXBUF];
        for (int count = 0; count < ConfValueEnum("banlist",&config_f); count++)
        {
                ConfValue("banlist","chan",count,CM,&config_f);
                if (match(name,CM))
                {
                        ConfValue("banlist","limit",count,CM,&config_f);
                        return atoi(CM);
                }
        }
        return 64;
}

void purge_empty_chans(userrec* u)
{

        int go_again = 1, purge = 0;

        // firstly decrement the count on each channel
        for (int f = 0; f < MAXCHANS; f++)
        {
                if (u->chans[f].channel)
                {
                        u->chans[f].channel->DelUser((char*)u);
                }
        }

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        if (!usercount(u->chans[i].channel))
                        {
                                chan_hash::iterator i2 = chanlist.find(u->chans[i].channel->name);
                                /* kill the record */
                                if (i2 != chanlist.end())
                                {
                                        log(DEBUG,"del_channel: destroyed: %s",i2->second->name);
                                        if (i2->second)
                                                delete i2->second;
                                        chanlist.erase(i2);
                                        go_again = 1;
                                        purge++;
                                        u->chans[i].channel = NULL;
                                }
                        }
                        else
                        {
                                log(DEBUG,"skipped purge for %s",u->chans[i].channel->name);
                        }
                }
        }
        log(DEBUG,"completed channel purge, killed %lu",(unsigned long)purge);

        DeleteOper(u);
}


char scratch[MAXBUF];
char sparam[MAXBUF];

char* chanmodes(chanrec *chan)
{
        if (!chan)
        {
                log(DEFAULT,"*** BUG *** chanmodes was given an invalid parameter");
                strcpy(scratch,"");
                return scratch;
        }

        strcpy(scratch,"");
        strcpy(sparam,"");
        if (chan->binarymodes & CM_NOEXTERNAL)
        {
                strlcat(scratch,"n",MAXMODES);
        }
        if (chan->binarymodes & CM_TOPICLOCK)
        {
                strlcat(scratch,"t",MAXMODES);
        }
        if (chan->key[0])
        {
                strlcat(scratch,"k",MAXMODES);
        }
        if (chan->limit)
        {
                strlcat(scratch,"l",MAXMODES);
        }
        if (chan->binarymodes & CM_INVITEONLY)
        {
                strlcat(scratch,"i",MAXMODES);
        }
        if (chan->binarymodes & CM_MODERATED)
        {
                strlcat(scratch,"m",MAXMODES);
        }
        if (chan->binarymodes & CM_SECRET)
        {
                strlcat(scratch,"s",MAXMODES);
        }
        if (chan->binarymodes & CM_PRIVATE)
        {
                strlcat(scratch,"p",MAXMODES);
        }
        if (chan->key[0])
        {
                strlcat(sparam," ",MAXBUF);
                strlcat(sparam,chan->key,MAXBUF);
        }
        if (chan->limit)
        {
                char foo[24];
                sprintf(foo," %lu",(unsigned long)chan->limit);
                strlcat(sparam,foo,MAXBUF);
        }
        if (*chan->custom_modes)
        {
                strlcat(scratch,chan->custom_modes,MAXMODES);
                for (int z = 0; chan->custom_modes[z] != 0; z++)
                {
                        std::string extparam = chan->GetModeParameter(chan->custom_modes[z]);
                        if (extparam != "")
                        {
                                strlcat(sparam," ",MAXBUF);
                                strlcat(sparam,extparam.c_str(),MAXBUF);
                        }
                }
        }
        log(DEBUG,"chanmodes: %s %s%s",chan->name,scratch,sparam);
        strlcat(scratch,sparam,MAXMODES);
        return scratch;
}


/* compile a userlist of a channel into a string, each nick seperated by
 * spaces and op, voice etc status shown as @ and + */

void userlist(userrec *user,chanrec *c)
{
        if ((!c) || (!user))
        {
                log(DEFAULT,"*** BUG *** userlist was given an invalid parameter");
                return;
        }

        snprintf(list,MAXBUF,"353 %s = %s :", user->nick, c->name);

        std::vector<char*> *ulist = c->GetUsers();
        for (int i = 0; i < ulist->size(); i++)
        {
                char* o = (*ulist)[i];
                userrec* otheruser = (userrec*)o;
                if ((!has_channel(user,c)) && (strchr(otheruser->modes,'i')))
                {
                        /* user is +i, and source not on the channel, does not show
                         * nick in NAMES list */
                        continue;
                }
                strlcat(list,cmode(otheruser,c),MAXBUF);
                strlcat(list,otheruser->nick,MAXBUF);
                strlcat(list," ",MAXBUF);
                if (strlen(list)>(480-NICKMAX))
                {
                        /* list overflowed into
                         * multiple numerics */
                        WriteServ(user->fd,"%s",list);
                        snprintf(list,MAXBUF,"353 %s = %s :", user->nick, c->name);
                }
        }
        /* if whats left in the list isnt empty, send it */
        if (list[strlen(list)-1] != ':')
        {
                WriteServ(user->fd,"%s",list);
        }
}

/* return a count of the users on a specific channel accounting for
 * invisible users who won't increase the count. e.g. for /LIST */

int usercount_i(chanrec *c)
{
        int count = 0;

        if (!c)
        {
                log(DEFAULT,"*** BUG *** usercount_i was given an invalid parameter");
                return 0;
        }

        strcpy(list,"");
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if (i->second)
                {
                        if (has_channel(i->second,c))
                        {
                                if (isnick(i->second->nick))
                                {
                                        if ((!has_channel(i->second,c)) && (strchr(i->second->modes,'i')))
                                        {
                                                /* user is +i, and source not on the channel, does not show
                                                 * nick in NAMES list */
                                                continue;
                                        }
                                        count++;
                                }
                        }
                }
        }
        log(DEBUG,"usercount_i: %s %lu",c->name,(unsigned long)count);
        return count;
}


int usercount(chanrec *c)
{
        if (!c)
        {
                log(DEFAULT,"*** BUG *** usercount was given an invalid parameter");
                return 0;
        }
        int count = c->GetUserCounter();
        log(DEBUG,"usercount: %s %lu",c->name,(unsigned long)count);
        return count;
}


// looks up a users password for their connection class (<ALLOW>/<DENY> tags)

char* Passwd(userrec *user)
{
        for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
        {
                if (match(user->host,i->host) && (i->type == CC_ALLOW))
                {
                        return i->pass;
                }
        }
        return "";
}

bool IsDenied(userrec *user)
{
        for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
        {
                if (match(user->host,i->host) && (i->type == CC_DENY))
                {
                        return true;
                }
        }
        return false;
}




/* sends out an error notice to all connected clients (not to be used
 * lightly!) */

void send_error(char *s)
{
        log(DEBUG,"send_error: %s",s);
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if (isnick(i->second->nick))
                {
                        WriteServ(i->second->fd,"NOTICE %s :%s",i->second->nick,s);
                }
                else
                {
                        // fix - unregistered connections receive ERROR, not NOTICE
                        Write(i->second->fd,"ERROR :%s",s);
                }
        }
}

void Error(int status)
{
        signal (SIGALRM, SIG_IGN);
        signal (SIGPIPE, SIG_IGN);
        signal (SIGTERM, SIG_IGN);
        signal (SIGABRT, SIG_IGN);
        signal (SIGSEGV, SIG_IGN);
        signal (SIGURG, SIG_IGN);
        signal (SIGKILL, SIG_IGN);
        log(DEFAULT,"*** fell down a pothole in the road to perfection ***");
        send_error("Error! Segmentation fault! save meeeeeeeeeeeeee *splat!*");
        Exit(status);
}

// this function counts all users connected, wether they are registered or NOT.
int usercnt(void)
{
        return clientlist.size();
}

// this counts only registered users, so that the percentages in /MAP don't mess up when users are sitting in an unregistered state
int registered_usercount(void)
{
        int c = 0;
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick))) c++;
        }
        return c;
}

int usercount_invisible(void)
{
        int c = 0;

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick)) && (strchr(i->second->modes,'i'))) c++;
        }
        return c;
}

int usercount_opers(void)
{
        int c = 0;

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick)) && (strchr(i->second->modes,'o'))) c++;
        }
        return c;
}

int usercount_unknown(void)
{
        int c = 0;

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (i->second->registered != 7))
                        c++;
        }
        return c;
}

long chancount(void)
{
        return chanlist.size();
}

long count_servs(void)
{
        int c = 0;
        for (int i = 0; i < 32; i++)
        {
                if (me[i] != NULL)
                {
                        for (vector<ircd_connector>::iterator j = me[i]->connectors.begin(); j != me[i]->connectors.end(); j++)
                        {
                                if (strcasecmp(j->GetServerName().c_str(),ServerName))
                                {
                                        c++;
                                }
                        }
                }
        }
        return c;
}

long servercount(void)
{
        return count_servs()+1;
}

long local_count()
{
        int c = 0;
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick)) && (!strcasecmp(i->second->server,ServerName))) c++;
        }
        return c;
}

void ShowMOTD(userrec *user)
{
        char buf[65536];
        std::string WholeMOTD = "";
        if (!MOTD.size())
        {
                WriteServ(user->fd,"422 %s :Message of the day file is missing.",user->nick);
                return;
        }
        snprintf(buf,65535,":%s 375 %s :- %s message of the day\r\n", ServerName, user->nick, ServerName);
        WholeMOTD = WholeMOTD + buf;
        for (int i = 0; i != MOTD.size(); i++)
        {
                snprintf(buf,65535,":%s 372 %s :- %s\r\n", ServerName, user->nick, MOTD[i].c_str());
                WholeMOTD = WholeMOTD + buf;
        }
        snprintf(buf,65535,":%s 376 %s :End of message of the day.\r\n", ServerName, user->nick);
        WholeMOTD = WholeMOTD + buf;
        // only one write operation
        user->AddWriteBuf(WholeMOTD);
        statsSent += WholeMOTD.length();
}

void ShowRULES(userrec *user)
{
        if (!RULES.size())
        {
                WriteServ(user->fd,"NOTICE %s :Rules file is missing.",user->nick);
                return;
        }
        WriteServ(user->fd,"NOTICE %s :%s rules",user->nick,ServerName);
        for (int i = 0; i != RULES.size(); i++)
        {
                                WriteServ(user->fd,"NOTICE %s :%s",user->nick,RULES[i].c_str());
        }
        WriteServ(user->fd,"NOTICE %s :End of %s rules.",user->nick,ServerName);
}

// this returns 1 when all modules are satisfied that the user should be allowed onto the irc server
// (until this returns true, a user will block in the waiting state, waiting to connect up to the
// registration timeout maximum seconds)
bool AllModulesReportReady(userrec* user)
{
        for (int i = 0; i <= MODCOUNT; i++)
        {
                int res = modules[i]->OnCheckReady(user);
                        if (!res)
                                return false;
        }
        return true;
}

char islast(const char* s)
{
        char c = '`';
        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (strcasecmp(me[j]->connectors[k].GetServerName().c_str(),s))
                                {
                                        c = '|';
                                }
                                if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),s))
                                {
                                        c = '`';
                                }
                        }
                }
        }
        return c;
}

long map_count(const char* s)
{
        int c = 0;
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick)) && (!strcasecmp(i->second->server,s))) c++;
        }
        return c;
}

void createcommand(char* cmd, handlerfunc f, char flags, int minparams,char* source)
{
        command_t comm;
        /* create the command and push it onto the table */
        strlcpy(comm.command,cmd,MAXBUF);
        strlcpy(comm.source,source,MAXBUF);
        comm.handler_function = f;
        comm.flags_needed = flags;
        comm.min_params = minparams;
        comm.use_count = 0;
        comm.total_bytes = 0;
        cmdlist.push_back(comm);
        log(DEBUG,"Added command %s (%lu parameters)",cmd,(unsigned long)minparams);
}


void SetupCommandTable(void)
{
        createcommand("USER",handle_user,0,4,"<core>");
        createcommand("NICK",handle_nick,0,1,"<core>");
        createcommand("QUIT",handle_quit,0,0,"<core>");
        createcommand("VERSION",handle_version,0,0,"<core>");
        createcommand("PING",handle_ping,0,1,"<core>");
        createcommand("PONG",handle_pong,0,1,"<core>");
        createcommand("ADMIN",handle_admin,0,0,"<core>");
        createcommand("PRIVMSG",handle_privmsg,0,2,"<core>");
        createcommand("INFO",handle_info,0,0,"<core>");
        createcommand("TIME",handle_time,0,0,"<core>");
        createcommand("WHOIS",handle_whois,0,1,"<core>");
        createcommand("WALLOPS",handle_wallops,'o',1,"<core>");
        createcommand("NOTICE",handle_notice,0,2,"<core>");
        createcommand("JOIN",handle_join,0,1,"<core>");
        createcommand("NAMES",handle_names,0,0,"<core>");
        createcommand("PART",handle_part,0,1,"<core>");
        createcommand("KICK",handle_kick,0,2,"<core>");
        createcommand("MODE",handle_mode,0,1,"<core>");
        createcommand("TOPIC",handle_topic,0,1,"<core>");
        createcommand("WHO",handle_who,0,1,"<core>");
        createcommand("MOTD",handle_motd,0,0,"<core>");
        createcommand("RULES",handle_rules,0,0,"<core>");
        createcommand("OPER",handle_oper,0,2,"<core>");
        createcommand("LIST",handle_list,0,0,"<core>");
        createcommand("DIE",handle_die,'o',1,"<core>");
        createcommand("RESTART",handle_restart,'o',1,"<core>");
        createcommand("KILL",handle_kill,'o',2,"<core>");
        createcommand("REHASH",handle_rehash,'o',0,"<core>");
        createcommand("LUSERS",handle_lusers,0,0,"<core>");
        createcommand("STATS",handle_stats,0,1,"<core>");
        createcommand("USERHOST",handle_userhost,0,1,"<core>");
        createcommand("AWAY",handle_away,0,0,"<core>");
        createcommand("ISON",handle_ison,0,0,"<core>");
        createcommand("SUMMON",handle_summon,0,0,"<core>");
        createcommand("USERS",handle_users,0,0,"<core>");
        createcommand("INVITE",handle_invite,0,0,"<core>");
        createcommand("PASS",handle_pass,0,1,"<core>");
        createcommand("TRACE",handle_trace,'o',0,"<core>");
        createcommand("WHOWAS",handle_whowas,0,1,"<core>");
        createcommand("CONNECT",handle_connect,'o',1,"<core>");
        createcommand("SQUIT",handle_squit,'o',0,"<core>");
        createcommand("MODULES",handle_modules,0,0,"<core>");
        createcommand("LINKS",handle_links,0,0,"<core>");
        createcommand("MAP",handle_map,0,0,"<core>");
        createcommand("KLINE",handle_kline,'o',1,"<core>");
        createcommand("GLINE",handle_gline,'o',1,"<core>");
        createcommand("ZLINE",handle_zline,'o',1,"<core>");
        createcommand("QLINE",handle_qline,'o',1,"<core>");
        createcommand("ELINE",handle_eline,'o',1,"<core>");
        createcommand("LOADMODULE",handle_loadmodule,'o',1,"<core>");
        createcommand("UNLOADMODULE",handle_unloadmodule,'o',1,"<core>");
        createcommand("SERVER",handle_server,0,0,"<core>");
}

bool DirValid(char* dirandfile)
{
        char work[MAXBUF];
        strlcpy(work,dirandfile,MAXBUF);
        int p = strlen(work);
        // we just want the dir
        while (strlen(work))
        {
                if (work[p] == '/')
                {
                        work[p] = '\0';
                        break;
                }
                work[p--] = '\0';
        }
        char buffer[MAXBUF], otherdir[MAXBUF];
        // Get the current working directory
        if( getcwd( buffer, MAXBUF ) == NULL )
                return false;
        chdir(work);
        if( getcwd( otherdir, MAXBUF ) == NULL )
                return false;
        chdir(buffer);
        if (strlen(otherdir) >= strlen(work))
        {
                otherdir[strlen(work)] = '\0';
                if (!strcmp(otherdir,work))
                {
                        return true;
                }
                return false;
        }
        else return false;
}

std::string GetFullProgDir(char** argv, int argc)
{
        char work[MAXBUF];
        strlcpy(work,argv[0],MAXBUF);
        int p = strlen(work);
        // we just want the dir
        while (strlen(work))
        {
                if (work[p] == '/')
                {
                        work[p] = '\0';
                        break;
                }
                work[p--] = '\0';
        }
        char buffer[MAXBUF], otherdir[MAXBUF];
        // Get the current working directory
        if( getcwd( buffer, MAXBUF ) == NULL )
                return "";
        chdir(work);
        if( getcwd( otherdir, MAXBUF ) == NULL )
                return "";
        chdir(buffer);
        return otherdir;
}

