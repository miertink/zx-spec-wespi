// Web page languages (see web_i18n.h).
//
// Texts go into HTML and into single-quoted JavaScript strings: no single
// quotes, no '<' or '&' in them.

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"
#include "web_i18n.h"

typedef struct {
    const char *key;
    const char *t[LANG_COUNT];          /* pt-BR, es-ES, en-US */
} entry_t;

static const entry_t texts[] = {
    /* header and status */
    { "PLAYING",     { "Tocando", "Reproduciendo", "Playing" } },
    { "LOOP",        { "Loop", "Bucle", "Loop" } },
    { "STOP",        { "Parar", "Detener", "Stop" } },
    { "STOPPED_AT",  { "Fita parada em", "Cinta detenida en", "Tape stopped at" } },
    { "STOP_MARK",   { "marca \"stop the tape\" do arquivo", "marca \"stop the tape\" del archivo",
                       "\"stop the tape\" mark in the file" } },
    { "CONTINUE",    { "Continuar", "Continuar", "Continue" } },
    { "PLAY_MENU",   { "Tocar menu", "Reproducir menú", "Play menu" } },
    { "IDLE",        { "Parado", "Detenido", "Idle" } },
    { "BLOCK",       { "bloco", "bloque", "block" } },
    { "BLOCK_C",     { "Bloco", "Bloque", "Block" } },
    { "OF",          { "de", "de", "of" } },
    { "IN_PROGRESS", { "em andamento", "en curso", "in progress" } },
    { "REMAINING",   { "restam ~", "quedan ~", "remaining ~" } },
    { "FILE_PCT",    { "arquivo", "archivo", "file" } },
    /* SAVE through MIC */
    { "REC_IN",      { "Recebendo do Spectrum", "Recibiendo del Spectrum", "Receiving from the Spectrum" } },
    { "REC",         { "Gravando", "Grabando", "Saving" } },
    { "REC_PILOT",   { "tom piloto", "tono piloto", "pilot tone" } },
    { "REC_WAIT",    { "aguardando os dados", "esperando los datos", "waiting for the data" } },
    { "SAVED",       { "Gravado", "Guardado", "Saved" } },
    { "REC_ERR",     { "Gravação falhou", "La grabación falló", "Save failed" } },
    { "E_CHECKSUM",  { "erro de leitura (checksum)", "error de lectura (checksum)", "read error (checksum)" } },
    { "E_NOHDR",     { "dados sem cabeçalho", "datos sin cabecera", "data without a header" } },
    /* block types: header types 0..3 */
    { "TYPE0",       { "programa", "programa", "program" } },
    { "TYPE1",       { "matriz num.", "matriz num.", "number array" } },
    { "TYPE2",       { "matriz texto", "matriz texto", "char array" } },
    { "TYPE3",       { "bytes", "bytes", "bytes" } },
    /* what a block is (tape_status desc codes, decoded by the page) */
    { "D_HDR",       { "cabeçalho", "cabecera", "header" } },
    { "D_ROM",       { "dados (ROM)", "datos (ROM)", "data (ROM)" } },
    { "D_PROG",      { "programa (LOADs sob demanda)", "programa (LOADs bajo demanda)",
                       "program (LOADs on request)" } },
    { "D_TURBO",     { "turbo do ESP, para", "turbo del ESP, a", "ESP turbo, to" } },
    { "D_GAME",      { "turbo (loader do jogo)", "turbo (cargador del juego)", "turbo (game loader)" } },
    { "D_PURE",      { "dados puros", "datos puros", "pure data" } },
    { "D_DIRECT",    { "gravação direta", "grabación directa", "direct recording" } },
    { "D_CSW",       { "gravação CSW", "grabación CSW", "CSW recording" } },
    { "D_GEN",       { "dados generalizados", "datos generalizados", "generalized data" } },
    { "D_DATA",      { "dados", "datos", "data" } },
    /* upload card */
    { "UP_TITLE",    { "Enviar arquivo", "Subir archivo", "Upload file" } },
    { "UP_NAME",     { "nome no servidor", "nombre en el servidor", "name on the server" } },
    { "UP_SEND",     { "Enviar", "Subir", "Upload" } },
    { "UP_SENDING",  { "Enviando...", "Subiendo...", "Uploading..." } },
    { "UP_CHOOSE",   { "Escolha um arquivo.", "Elige un archivo.", "Choose a file." } },
    { "UP_NONAME",   { "Informe o nome no servidor.", "Indica el nombre en el servidor.",
                       "Enter the name on the server." } },
    { "UP_BIG",      { "Arquivo maior que 200 KB.", "Archivo mayor de 200 KB.", "File larger than 200 KB." } },
    { "UP_ERR",      { "Erro", "Error", "Error" } },
    { "UP_MANY",     { "%d arquivos: cada um com o seu nome", "%d archivos: cada uno con su nombre",
                       "%d files: each keeps its name" } },
    { "UP_BIG2",     { "maior que 200 KB", "mayor de 200 KB", "larger than 200 KB" } },
    { "UP_FAILED",   { "Não enviados:", "No subidos:", "Not uploaded:" } },
    { "UP_LOST",     { "A conexão caiu.", "Se perdió la conexión.", "Connection lost." } },
    /* file table */
    { "TAPES",       { "Fitas", "Cintas", "Tapes" } },
    { "COL_FILE",    { "Arquivo", "Archivo", "File" } },
    { "COL_TYPE",    { "Tipo", "Tipo", "Type" } },
    { "COL_SIZE",    { "Tamanho", "Tamaño", "Size" } },
    { "EMPTY",       { "Nenhum arquivo. Envie um .tap, .tzx, .pzx ou .csw.",
                       "Ningún archivo. Sube un .tap, .tzx, .pzx o .csw.",
                       "No files. Upload a .tap, .tzx, .pzx or .csw." } },
    { "SYSFILES",    { "Arquivos do sistema", "Archivos del sistema", "System files" } },
    { "IN_FILE",     { "no arquivo:", "en el archivo:", "in the file:" } },
    { "PLAY",        { "Play", "Play", "Play" } },
    { "RENAME",      { "Renomear", "Renombrar", "Rename" } },
    { "DELETE",      { "Excluir", "Eliminar", "Delete" } },
    { "DELETE_Q",    { "Excluir", "¿Eliminar", "Delete" } },
    /* footer */
    { "STORAGE",     { "Armazenamento: %u KB livres de %u KB", "Almacenamiento: %u KB libres de %u KB",
                       "Storage: %u KB free of %u KB" } },
    { "VERSION",     { "versão", "versión", "version" } },
    { "DIAG",        { "Diagnóstico", "Diagnóstico", "Diagnostics" } },
    { "MARK_LOG",    { "Marcar log", "Marcar log", "Mark log" } },
    /* errors shown to the user */
    { "E_NOFILE",    { "Arquivo inexistente", "El archivo no existe", "File does not exist" } },
    { "E_EXISTS",    { "Já existe um arquivo com esse nome", "Ya existe un archivo con ese nombre",
                       "A file with that name already exists" } },
    { "E_BADNAME",   { "Nome inválido: use letras, números, - e _", "Nombre no válido: usa letras, números, - y _",
                       "Invalid name: use letters, digits, - and _" } },
    { "E_PLAYING",   { "O arquivo está tocando: pare a fita antes", "El archivo se está reproduciendo: detén la cinta antes",
                       "The file is playing: stop the tape first" } },
    { "E_PROTECTED", { "Arquivo do sistema: o menu é gerado pelo ESP e não pode ser alterado",
                       "Archivo del sistema: el menú lo genera el ESP y no se puede cambiar",
                       "System file: the menu is made by the ESP and cannot be changed" } },
    { "E_RENAME",    { "Falha ao renomear", "Error al renombrar", "Rename failed" } },
    { "E_NOTAPE",    { "Não é um arquivo de fita (.tap .tzx .pzx .csw)", "No es un archivo de cinta (.tap .tzx .pzx .csw)",
                       "Not a tape file (.tap .tzx .pzx .csw)" } },
    { "E_PLAY",      { "Não foi possível tocar o arquivo", "No se pudo reproducir el archivo", "Cannot play the file" } },
    { "E_BUSY",      { "A fita não parou a tempo, tente de novo", "La cinta no se detuvo a tiempo, inténtalo de nuevo",
                       "The tape did not stop in time, try again" } },
    { "E_NAMELEN",   { "Nome de arquivo longo demais", "Nombre de archivo demasiado largo", "File name too long" } },
    { "E_UPLOAD",    { "Falha ao gravar o arquivo", "Error al guardar el archivo", "Failed to store the file" } },
};

static const char *const tags[LANG_COUNT]  = { "pt-BR", "es-ES", "en-US" };
static const char *const codes[LANG_COUNT] = { "pt", "es", "en" };

const char *i18n_tag(lang_t l)  { return tags[l < LANG_COUNT ? l : LANG_EN]; }
const char *i18n_code(lang_t l) { return codes[l < LANG_COUNT ? l : LANG_EN]; }

bool i18n_parse(const char *code, lang_t *out)
{
    for (int i = 0; i < LANG_COUNT; i++) {
        if (strncasecmp(code, codes[i], 2) == 0) {
            *out = (lang_t)i;
            return true;
        }
    }
    return false;
}

const char *tr(lang_t l, const char *key)
{
    if (l >= LANG_COUNT) l = LANG_EN;
    if (strcmp(key, "LANG_TAG") == 0) return tags[l];
    if (strcmp(key, "LANG") == 0) return codes[l];
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (strcmp(texts[i].key, key) == 0) return texts[i].t[l];
    }
    return key;
}

/* The choice, cached: NVS "zxstorage" / "lang" (0 PT, 1 ES, 2 EN) */
#define NVS_NS  "zxstorage"
#define NVS_KEY "lang"
static int chosen = -2;                 /* -2 not read yet, -1 none */

bool i18n_chosen(lang_t *out)
{
    if (chosen == -2) {
        nvs_handle h;
        uint8_t v = 0xFF;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, NVS_KEY, &v) != ESP_OK) v = 0xFF;
            nvs_close(h);
        }
        chosen = v < LANG_COUNT ? v : -1;
    }
    if (chosen < 0) return false;
    *out = (lang_t)chosen;
    return true;
}

lang_t i18n_menu_lang(void)
{
    lang_t l;
    return i18n_chosen(&l) ? l : LANG_EN;
}

void i18n_choose(lang_t l)
{
    if (l >= LANG_COUNT) return;
    chosen = l;
    nvs_handle h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, NVS_KEY, l) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI("i18n", "language: %s", tags[l]);
}

lang_t i18n_lang(httpd_req_t *req)
{
    char buf[128];
    lang_t l;
    if (i18n_chosen(&l)) return l;
    /* Accept-Language: the first of our languages in the browser's order,
     * e.g. "es-ES,es;q=0.9,en;q=0.8" (the list is ordered by preference) */
    if (httpd_req_get_hdr_value_str(req, "Accept-Language", buf, sizeof(buf)) == ESP_OK
        || httpd_req_get_hdr_value_len(req, "Accept-Language") > 0) {
        buf[sizeof(buf) - 1] = 0;
        for (char *p = buf; *p;) {
            while (*p == ' ' || *p == ',') p++;
            if (i18n_parse(p, &l)) return l;
            p = strchr(p, ',');
            if (!p) break;
        }
    }
    return LANG_EN;
}

void i18n_send_tpl(i18n_out_t out, const char *s, size_t n, lang_t l)
{
    const char *end = s + n, *p = s;
    while (p < end) {
        const char *o = memchr(p, '{', end - p);
        if (!o || o + 1 >= end) break;
        if (o[1] != '{') {
            p = o + 1;
            continue;
        }
        const char *c = o + 2;
        const char *e = c;
        while (e + 1 < end && !(e[0] == '}' && e[1] == '}') && e - c < 24) e++;
        if (e + 1 >= end || e[0] != '}' || e[1] != '}') {
            p = o + 2;
            continue;
        }
        char key[25];
        memcpy(key, c, e - c);
        key[e - c] = 0;
        if (o > s) out(s, o - s);
        const char *t = tr(l, key);
        out(t, strlen(t));
        s = p = e + 2;
    }
    if (end > s) out(s, end - s);
}
