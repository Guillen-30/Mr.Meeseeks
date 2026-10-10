#!/bin/bash
# ============================================================================
# resumen.sh - Resume un CSV de experimentos (Tarea 3, Mr. Meeseeks)
#
# Uso:  ./resumen.sh archivo.csv [columnas_clave]
#
# Agrupa las filas por las columnas clave (por defecto la 4 = dificultad) y,
# para cada grupo, muestra: cantidad de corridas, porcentaje terminado y
# media (desviacion) [minimo-maximo] de cada medida.
#
# Columnas del CSV:
#   1 id   2 tipo   3 solicitud   4 dificultad   5 duracion_real_s
#   6 espera_simulada_s   7 estado   8 meeseeks   9 nivel_max   10 terminados
#   11 exceso_s   12 factor   13 limite_meeseeks   14 tiempo_max_s
#
# Ejemplos:
#   ./resumen.sh resultados/A_dificultad.csv          (por dificultad)
#   ./resumen.sh resultados/C_tipos.csv 3             (por solicitud)
#   ./resumen.sh resultados/E_factor.csv 12,4         (por factor y dificultad)
# ============================================================================

file=$1
keys=${2:-4}

if [ ! -f "$file" ]; then
    echo "Uso: $0 archivo.csv [columnas_clave]"
    exit 1
fi

printf '%-30s %3s %5s | %-24s | %-16s | %-10s | %-22s | %-8s\n' \
    "grupo" "n" "ok" "Meeseeks media(sd)[min-max]" "niveles media(sd)" \
    "terminados" "tiempo real s media(sd)" "exceso s"

awk -F, -v keys="$keys" '
BEGIN {
    nk = split(keys, K, ",")
    split("5 8 9 10 11", C, " ")     # duracion, meeseeks, niveles, terminados, exceso
}
NR == 1 { next }
{
    key = ""
    for (i = 1; i <= nk; i++)
        key = key (i > 1 ? " " : "") $(K[i])
    n[key]++
    ok[key] += ($7 == "TERMINADA")
    for (j = 1; j <= 5; j++) {
        v = $(C[j]) + 0
        s[key, j] += v
        q[key, j] += v * v
        if (n[key] == 1 || v < mn[key, j]) mn[key, j] = v
        if (n[key] == 1 || v > mx[key, j]) mx[key, j] = v
    }
}
function mean(k, j)  { return s[k, j] / n[k] }
function sd(k, j,   d) {
    if (n[k] < 2) return 0
    d = (q[k, j] - s[k, j] * s[k, j] / n[k]) / (n[k] - 1)
    return d > 0 ? sqrt(d) : 0
}
END {
    for (k in n) {
        c1 = sprintf("%.1f (%.1f) [%d-%d]", mean(k, 2), sd(k, 2), mn[k, 2], mx[k, 2])
        c2 = sprintf("%.2f (%.2f)", mean(k, 3), sd(k, 3))
        c3 = sprintf("%.1f", mean(k, 4))
        c4 = sprintf("%.4f (%.4f)", mean(k, 1), sd(k, 1))
        c5 = sprintf("%.3f", mean(k, 5))
        printf "%-30s %3d %4.0f%% | %-24s | %-16s | %-10s | %-22s | %-8s\n",
            k, n[k], 100 * ok[k] / n[k], c1, c2, c3, c4, c5
    }
}' "$file" | LC_ALL=C sort -V
