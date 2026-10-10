#!/bin/bash
# ============================================================================
# experimentos.sh - Experimentos del punto 0x02 (Tarea 3, Mr. Meeseeks)
#
# Uso:  ./experimentos.sh [A|B|C|D|E|R|resumen|todos]
#
# Cada experimento alimenta al Box con una lista de solicitudes y deja un CSV
# en resultados/ (opcion -o del programa). Para ver un resumen:
#       ./resumen.sh resultados/A_dificultad.csv
#
# Experimentos:
#   A  Dificultad contra cantidad de Meeseeks, niveles y tiempo real.
#      Espera simulada minima (t=0.05) para que corra rapido.
#   B  Igual que A pero con tiempos de espera al azar (0.5 a 5 s), como en
#      una ejecucion normal. Muestra la espera simulada acumulada.
#   C  Comparacion de los tres tipos de solicitud: calc, run y texto.
#   D  Tiempo maximo: que pasa si una solicitud no termina a tiempo.
#        D_tiempo_extra.csv  modo -N (no se elimina a nadie): tiempo adicional
#        D_caos.csv          caos planetario (se elimina todo al vencer)
#        D_imposible.csv     dificultad 0 con limite de 100 Meeseeks
#   E  Efecto del factor de coordinacion (-f) en la cantidad de Meeseeks.
#   R  Ejecucion realista: dificultad al azar (distribucion normal) y tiempos
#      al azar.
#
#   resumen  junta las tablas de todos los experimentos en resultados/resumen.txt
#
# Las repeticiones se pueden cambiar con variables de entorno, por ejemplo:
#       REPS_A=5 ./experimentos.sh A
# ============================================================================

BIN=./meeseeks
OUT=resultados
mkdir -p "$OUT"

REPS_A=${REPS_A:-10}
REPS_B=${REPS_B:-3}
REPS_C=${REPS_C:-20}
REPS_D=${REPS_D:-3}
REPS_E=${REPS_E:-10}
REPS_R=${REPS_R:-20}

if [ ! -x "$BIN" ]; then
    echo "No se encuentra $BIN. Compile primero con: make"
    exit 1
fi

# --- A: dificultad vs Meeseeks y tiempo (espera simulada minima) ------------
exp_A() {
    local f="$OUT/A_dificultad.csv"
    rm -f "$f"
    echo "[A] Dificultad (t=0.05), $REPS_A repeticiones por valor..."
    {
        for d in 1 10 20 30 40 50 60 70 80 90; do
            for r in $(seq 1 "$REPS_A"); do
                echo "dif=$d,t=0.05: experimento A"
            done
        done
        echo salir
    } | "$BIN" -m 1000 -o "$f" > /dev/null
}

# --- B: igual que A pero con tiempos de espera al azar ----------------------
exp_B() {
    local f="$OUT/B_tiempos_reales.csv"
    rm -f "$f"
    echo "[B] Tiempos al azar, $REPS_B repeticiones por valor..."
    {
        for d in 20 40 60 80; do
            for r in $(seq 1 "$REPS_B"); do
                echo "dif=$d: experimento B"
            done
        done
        echo salir
    } | "$BIN" -m 1000 -T 600 -o "$f" > /dev/null
}

# --- C: calc vs run vs texto -------------------------------------------------
exp_C() {
    local f="$OUT/C_tipos.csv"
    rm -f "$f"
    echo "[C] Tipos de solicitud, $REPS_C repeticiones de cada una..."
    {
        for r in $(seq 1 "$REPS_C"); do
            echo "calc:28+18/74*321"
            echo "run:ls"
            echo "run:sleep 0.1"
            echo "dif=90,t=0.05: texto facil"
            echo "dif=40,t=0.05: texto dificil"
        done
        echo salir
    } | "$BIN" -m 1000 -o "$f" > /dev/null
}

# --- D: tiempo maximo ---------------------------------------------------------
exp_D() {
    local fn="$OUT/D_tiempo_extra.csv"
    local fc="$OUT/D_caos.csv"
    local fi="$OUT/D_imposible.csv"
    rm -f "$fn" "$fc" "$fi"

    echo "[D1] Modo -N con tiempo maximo de 5 s: cuanto tiempo adicional hace falta..."
    {
        for d in 1 20 40 60; do
            for r in $(seq 1 "$REPS_D"); do
                echo "dif=$d: experimento D"
            done
        done
        echo salir
    } | "$BIN" -m 1000 -T 5 -N -o "$fn" > /dev/null

    echo "[D2] Caos planetario con tiempo maximo de 5 s (una corrida por solicitud)..."
    for d in 1 20 40 60; do
        for r in $(seq 1 "$REPS_D"); do
            printf 'dif=%s: experimento D\nsalir\n' "$d" | "$BIN" -m 1000 -T 5 -o "$fc" > /dev/null
        done
    done

    echo "[D3] Dificultad 0 (imposible), limite de 100 Meeseeks, modo -N..."
    for r in $(seq 1 "$REPS_D"); do
        printf 'dif=0: imposible\nsalir\n' | "$BIN" -m 100 -T 5 -N -o "$fi" > /dev/null
    done
}

# --- E: factor de coordinacion ---------------------------------------------------
exp_E() {
    local f="$OUT/E_factor.csv"
    rm -f "$f"
    echo "[E] Factor de coordinacion, $REPS_E repeticiones..."
    for fac in 1.0 1.2 1.5 1.8 2.0; do
        {
            for d in 40 60; do
                for r in $(seq 1 "$REPS_E"); do
                    echo "dif=$d,t=0.05: experimento E"
                done
            done
            echo salir
        } | "$BIN" -m 300 -f "$fac" -o "$f" > /dev/null
    done
}

# --- R: ejecucion realista -----------------------------------------------------------
exp_R() {
    local f="$OUT/R_realista.csv"
    rm -f "$f"
    echo "[R] Ejecucion realista (dificultad y tiempos al azar), $REPS_R solicitudes..."
    {
        for r in $(seq 1 "$REPS_R"); do
            echo "solicitud realista $r"
        done
        echo salir
    } | "$BIN" -m 1000 -T 600 -o "$f" > /dev/null
}

# --- resumen: junta las tablas de todos los experimentos en un solo archivo ---
resumenes() {
    local f="$OUT/resumen.txt"
    {
        echo "== A: dificultad contra Meeseeks y tiempo (espera simulada t=0.05) =="
        ./resumen.sh "$OUT/A_dificultad.csv" 4
        echo
        echo "== B: igual que A con tiempos de espera al azar =="
        ./resumen.sh "$OUT/B_tiempos_reales.csv" 4
        echo "(espera simulada media por dificultad)"
        awk -F, 'NR>1{s[$4]+=$6;c[$4]++} END{for(k in s) printf "  dif %s: %.2f s\n", k, s[k]/c[k]}' \
            "$OUT/B_tiempos_reales.csv" | sort -V
        echo
        echo "== C: tipos de solicitud (calc, run, texto) =="
        ./resumen.sh "$OUT/C_tipos.csv" 3
        echo
        echo "== D1: modo -N con tiempo maximo de 5 s (tiempo adicional) =="
        ./resumen.sh "$OUT/D_tiempo_extra.csv" 4
        echo "(detalle: tiempo total = espera simulada + tiempo real)"
        awk -F, 'NR>1{printf "  dif %s: total %.2f s, exceso %.2f s, %s, Meeseeks %s\n",$4,$5+$6,$11,$7,$8}' \
            "$OUT/D_tiempo_extra.csv"
        echo
        echo "== D2: caos planetario con tiempo maximo de 5 s =="
        awk -F, 'NR>1{printf "  dif %s: %s, Meeseeks %s, terminados por orden %s, nivel %s\n",$4,$7,$8,$10,$9}' \
            "$OUT/D_caos.csv"
        echo
        echo "== D3: dificultad 0 (imposible), limite de 100 Meeseeks, modo -N =="
        awk -F, 'NR>1{printf "  %s, Meeseeks %s, niveles %s, total %.2f s, exceso %.2f s\n",$7,$8,$9,$5+$6,$11}' \
            "$OUT/D_imposible.csv"
        echo
        echo "== E: factor de coordinacion (grupo = factor y dificultad) =="
        ./resumen.sh "$OUT/E_factor.csv" 12,4
        echo
        echo "== R: ejecucion realista (dificultad y tiempos al azar) =="
        echo "dificultad | Meeseeks | niveles | terminados por orden | espera simulada s | total s | estado"
        awk -F, 'NR>1{printf "%7.2f | %4d | %2d | %4d | %7.2f | %7.2f | %s\n",$4,$8,$9,$10,$6,$5+$6,$7}' \
            "$OUT/R_realista.csv" | sort -n
    } > "$f"
    echo "Resumen guardado en $f"
}

case "${1:-todos}" in
    A) exp_A ;;
    B) exp_B ;;
    C) exp_C ;;
    D) exp_D ;;
    E) exp_E ;;
    R) exp_R ;;
    resumen) resumenes ;;
    todos) exp_A; exp_C; exp_E; exp_B; exp_D; exp_R; resumenes ;;
    *) echo "Uso: $0 [A|B|C|D|E|R|resumen|todos]"; exit 1 ;;
esac
echo "Listo. Resultados en $OUT/"
