/*
 * mod-sanctuary-gm - script loader
 *
 * The name of this function is derived from the module directory name by modules/CMakeLists.txt;
 * do not rename it.
 */

void AddSC_sanctuary_gm_scripts();
void AddSC_sanctuary_command_policy();
void AddSC_sanctuary_gm_class();

void Addmod_sanctuary_gmScripts()
{
    AddSC_sanctuary_gm_scripts();
    AddSC_sanctuary_command_policy();
    AddSC_sanctuary_gm_class();
}
