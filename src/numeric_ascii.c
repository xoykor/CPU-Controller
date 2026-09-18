/*
 * CPU Switch Control - numeric_ascii.c
 *
 * Conversão numérica independente da localidade da interface.
 *
 * GTK respeita a localidade do usuário, então um GtkSpinButton pode exibir
 * "-60,00" em português. Isso é desejável para a GUI. Já formatos de máquina
 * como JSON e intel-undervolt usam ponto decimal obrigatoriamente.
 *
 * Em vez de alterar a localidade global do processo, usamos uma localidade "C"
 * temporária e limitada à conversão necessária.
 */

#define _GNU_SOURCE

#include "numeric_ascii.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

/* Cria uma localidade numérica ASCII. A localidade "C" existe em todo sistema POSIX. */
static locale_t create_ascii_numeric_locale(void) {
    return newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

double numeric_ascii_strtod(const char *text, char **end_ptr) {
    locale_t ascii_locale = create_ascii_numeric_locale();
    if (ascii_locale == (locale_t)0) {
        /*
         * Falhar aqui é extremamente improvável, mas manter o contrato de
         * strtod() é melhor do que interpretar silenciosamente com a locale do
         * usuário e transformar 60.50 em 60.
         */
        if (end_ptr != NULL) {
            *end_ptr = (char *)text;
        }
        errno = EINVAL;
        return 0.0;
    }

    double value = strtod_l(text, end_ptr, ascii_locale);
    int saved_errno = errno;
    freelocale(ascii_locale);
    errno = saved_errno;
    return value;
}

int numeric_ascii_format_double(char *buffer,
                                size_t buffer_size,
                                unsigned int precision,
                                double value) {
    locale_t ascii_locale = create_ascii_numeric_locale();
    if (ascii_locale == (locale_t)0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * uselocale() afeta somente a thread atual. Portanto a GUI continua
     * mostrando números localizados enquanto este pequeno trecho escreve
     * formatos de máquina com ponto decimal.
     */
    locale_t previous_locale = uselocale(ascii_locale);
    if (previous_locale == (locale_t)0) {
        int saved_errno = errno;
        freelocale(ascii_locale);
        errno = saved_errno;
        return -1;
    }

    int written = snprintf(buffer, buffer_size, "%.*f", (int)precision, value);

    int saved_errno = errno;
    (void)uselocale(previous_locale);
    freelocale(ascii_locale);
    errno = saved_errno;

    return written;
}
