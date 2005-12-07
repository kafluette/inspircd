// operjoin module by typobox43

using namespace std;

#include "users.h"
#include "channels.h"
#include "modules.h"
#include "helperfuncs.h"

/* $ModDesc: Forces opers to join a specified channel on oper-up */

class ModuleOperjoin : public Module
{
	private:
		std::string operChan;
		ConfigReader* conf;
		Server* Srv;

	public:
		ModuleOperjoin(Server* Me)
			: Module::Module(Me)
		{
			Srv = Me;
			conf = new ConfigReader;
			operChan = conf->ReadValue("operjoin", "channel", 0);
		}

		virtual ~ModuleOperjoin()
		{
			delete conf;
		}

		virtual Version GetVersion()
		{
			return Version(1,0,0,1,VF_VENDOR);
		}

		virtual void OnOper(userrec* user, std::string opertype)
		{
			if(operChan != "")
			{
				Srv->JoinUserToChannel(user,operChan,"");
			}

		}

};

class ModuleOperjoinFactory : public ModuleFactory
{
	public:
	        ModuleOperjoinFactory()
        	{
	        }

	        ~ModuleOperjoinFactory()
        	{
	        }

	        virtual Module * CreateModule(Server* Me)
        	{
                	return new ModuleOperjoin(Me);
	        }
};

extern "C" void * init_module( void )
{
        return new ModuleOperjoinFactory;
}

