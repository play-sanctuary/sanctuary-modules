/*
 * mod-proximity-voice - script loader
 *
 * The name of this function is derived from the module directory name by
 * modules/CMakeLists.txt; do not rename it.
 */

void AddSC_proximity_voice_scripts();
void AddSC_proximity_voice_commandscript();

void Addmod_proximity_voiceScripts()
{
    AddSC_proximity_voice_scripts();
    AddSC_proximity_voice_commandscript();
}
