/*
 * CPU Switch Control - numeric_ascii.h
 *
 * Helpers para números armazenados em formatos de máquina. A interface pode
 * usar a localidade do usuário (por exemplo, vírgula decimal em pt_BR), mas
 * JSON e intel-undervolt exigem ponto decimal. Estas funções mantêm essa
 * separação explícita.
 */

#ifndef CPU_SWITCH_NUMERIC_ASCII_H
#define CPU_SWITCH_NUMERIC_ASCII_H

#include <stddef.h>

/*
 * Equivalente a strtod(), mas sempre interpreta ponto como separador decimal.
 * END_PTR recebe o primeiro caractere não consumido, como em strtod().
 */
double numeric_ascii_strtod(const char *text, char **end_ptr);

/*
 * Formata VALUE com PRECISION casas decimais usando ponto, independentemente
 * de LC_NUMERIC. Retorna o mesmo tipo de resultado de snprintf().
 */
int numeric_ascii_format_double(char *buffer,
                                size_t buffer_size,
                                unsigned int precision,
                                double value);

#endif
