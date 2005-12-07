/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  Inspire is copyright (C) 2002-2004 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

using namespace std;

#include <stdio.h>
#include <string>
#include "users.h"
#include "channels.h"
#include "modules.h"

/* $ModDesc: Provides support for unreal-style SAJOIN command */

Server *Srv;
	 
void handle_sajoin(char **parameters, int pcnt, userrec *user)
{
	userrec* dest = Srv->FindNick(std::string(parameters[0]));
	if (dest)
	{
		/* might be nicer to make checking valid channel names an api function sometime --w00t */
		if (parameters[1][0] != '#')
		{
			/* we didn't need to check this for each character ;) */
			Srv->SendTo(NULL,user,"NOTICE "+std::string(user->nick)+" :*** Invalid characters in channel name");
			return;
		}

		for (unsigned int x = 0; x < strlen(parameters[1]); x++)
		{
				if ((parameters[1][x] == ' ') || (parameters[1][x] == ','))
				{
					Srv->SendTo(NULL,user,"NOTICE "+std::string(user->nick)+" :*** Invalid characters in channel name");
					return;
				}
		}

		Srv->SendOpers(std::string(user->nick)+" used SAJOIN to make "+std::string(dest->nick)+" join "+parameters[1]);
		Srv->JoinUserToChannel(dest,std::string(parameters[1]),std::string(dest->nick));
	}
}


class ModuleSajoin : public Module
{
 public:
	ModuleSajoin(Server* Me)
		: Module::Module(Me)
	{
		Srv = Me;
		Srv->AddCommand("SAJOIN",handle_sajoin,'o',2,"m_sajoin.so");
	}
	
	virtual ~ModuleSajoin()
	{
	}
	
	virtual Version GetVersion()
	{
		return Version(1,0,0,1,VF_VENDOR);
	}
	
};

// stuff down here is the module-factory stuff. For basic modules you can ignore this.

class ModuleSajoinFactory : public ModuleFactory
{
 public:
	ModuleSajoinFactory()
	{
	}
	
	~ModuleSajoinFactory()
	{
	}
	
	virtual Module * CreateModule(Server* Me)
	{
		return new ModuleSajoin(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSajoinFactory;
}

