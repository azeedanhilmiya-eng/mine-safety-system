# -*- coding: utf-8 -*-
import io, sys
p = r"lib\ESP_SR\src\esp32-hal-sr.c"
s = io.open(p, encoding="utf-8").read()

A_old = '  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);'
A_new = (
'  /* [PATCHED] Prefer the Chinese MultiNet model; fall back to English if absent. */\n'
'  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);\n'
'  bool mn_is_chinese = (mn_name != NULL);\n'
'  if (mn_name == NULL) {\n'
'    mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);\n'
'  }\n'
'  if (mn_name == NULL) {\n'
'    log_e("No MultiNet model found in the \'model\' partition");\n'
'    goto err;\n'
'  }'
)
assert s.count(A_old) == 1, "patch A anchor not unique: %d" % s.count(A_old)
s = s.replace(A_old, A_new)

B_old = '''  for (size_t i = 0; i < cmd_number; i++) {
    char *phonemes = flite_g2p(sr_commands[i].str, 1);
    if (phonemes == NULL) {
      log_e("failed to generate phonemes for cmd[%d] phrase[%lu]:'%s'", sr_commands[i].command_id, (unsigned long)i, sr_commands[i].str);
      continue;
    }
    esp_mn_commands_phoneme_add(sr_commands[i].command_id, (const char *)(sr_commands[i].str), phonemes);
    free(phonemes);
    log_i("  cmd[%d] phrase[%lu]:'%s'", sr_commands[i].command_id, (unsigned long)i, sr_commands[i].str);
  }'''
B_new = '''  for (size_t i = 0; i < cmd_number; i++) {
    /* [PATCHED] Chinese MultiNet takes pinyin directly (e.g. "jiu ming");
     * only the English model needs the flite grapheme-to-phoneme step. */
    if (mn_is_chinese) {
      esp_err_t add_err = esp_mn_commands_add(sr_commands[i].command_id, sr_commands[i].str);
      if (add_err != ESP_OK) {
        log_e("failed to add cmd[%d] phrase[%lu]:'%s' (%s)", sr_commands[i].command_id, (unsigned long)i, sr_commands[i].str, esp_err_to_name(add_err));
        continue;
      }
    } else {
      char *phonemes = flite_g2p(sr_commands[i].str, 1);
      if (phonemes == NULL) {
        log_e("failed to generate phonemes for cmd[%d] phrase[%lu]:'%s'", sr_commands[i].command_id, (unsigned long)i, sr_commands[i].str);
        continue;
      }
      esp_mn_commands_phoneme_add(sr_commands[i].command_id, (const char *)(sr_commands[i].str), phonemes);
      free(phonemes);
    }
    log_i("  cmd[%d] phrase[%lu]:'%s'", sr_commands[i].command_id, (unsigned long)i, sr_commands[i].str);
  }'''
assert s.count(B_old) == 1, "patch B anchor not unique: %d" % s.count(B_old)
s = s.replace(B_old, B_new)

# flite_g2p.h is still needed for the English fallback path
io.open(p, "w", encoding="utf-8").write(s)
print("patched OK")
