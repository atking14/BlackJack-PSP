#include <pspkernel.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgum.h>
#include <pspdebug.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

PSP_MODULE_INFO("Blackjack", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_VFPU | THREAD_ATTR_USER);

#define BUFFER_WIDTH 512
#define BUFFER_HEIGHT 272
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272

char list[0x20000] __attribute__((aligned(64)));
int running = 1;

// --- ESTRUCTURAS ---
typedef struct {
    int valor;      // 1-13 (AS=1, 2-10=valor, J=11, Q=12, K=13 - pero J/Q/K valen 10 puntos)
    int palo;       // 0-3 (Corazones, Diamantes, Tréboles, Picas)
    int visible;    // Para carta oculta del dealer
} Carta;

typedef struct {
    Carta cartas[12];
    int numCartas;
    int total;
    int ases;
} Mano;

typedef struct {
    Carta mazo[52];
    int cartasRestantes;
} Mazo;

typedef enum {
    ESTADO_MENU,
    ESTADO_APUESTA,      // Nuevo estado para elegir apuesta
    ESTADO_REPARTIENDO,
    ESTADO_JUGADOR,
    ESTADO_DEALER,
    ESTADO_RESULTADO,
} EstadoJuego;

typedef struct {
    float x, y, z;
} Vertex;

// --- VARIABLES GLOBALES ---
EstadoJuego estadoActual = ESTADO_MENU;
Mazo mazo;
Mano jugador;
Mano dealer;
char mensajeResultado[100];
int frameDelay = 0;
unsigned int bgColor = 0xFF2D5016; // Verde mesa de casino

// Sistema de apuestas
int balanceJugador = 5000;
int balanceCasa = 100000;
int apuestaActual = 50;
int apuestaMinima = 50;
int ganancias = 0;  // Ganancias de la última mano

// Colores mejorados
unsigned int colorRojo = 0xFF0000FF;
unsigned int colorNegro = 0xFF000000;
unsigned int colorBlanco = 0xFFFFFFFF;
unsigned int colorCartaReverso = 0xFFE1B1FF; // Rosa
unsigned int colorBorde = 0xFF333333;   

// --- FUNCIONES DE SISTEMA ---
int exit_callback(int arg1, int arg2, void *common) {
    running = 0;
    return 0;
}

int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) sceKernelStartThread(thid, 0, 0);
}

// --- FUNCIONES DE GRÁFICOS ---
void initGu() {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void*)0x88000, BUFFER_WIDTH);
    sceGuDepthBuffer((void*)0x110000, BUFFER_WIDTH);
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, -1, 1);

    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);
    
    // Inicializar debug screen
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
}

void drawRect(float x, float y, float w, float h, unsigned int color) {
    Vertex* vertices = (Vertex*)sceGuGetMemory(4 * sizeof(Vertex));

    vertices[0].x = x;     vertices[0].y = y;     vertices[0].z = 0.0f;
    vertices[1].x = x + w; vertices[1].y = y;     vertices[1].z = 0.0f;
    vertices[2].x = x;     vertices[2].y = y + h; vertices[2].z = 0.0f;
    vertices[3].x = x + w; vertices[3].y = y + h; vertices[3].z = 0.0f;

    sceGuColor(color);
    sceGumDrawArray(GU_TRIANGLE_STRIP, GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, 0, vertices);
}

// --- FUNCIONES DE LÓGICA DE BLACKJACK ---
void inicializarMazo() {
    int index = 0;
    for(int palo = 0; palo < 4; palo++) {
        for(int valor = 1; valor <= 13; valor++) {
            mazo.mazo[index].valor = valor;
            mazo.mazo[index].palo = palo;
            mazo.mazo[index].visible = 1;
            index++;
        }
    }
    mazo.cartasRestantes = 52;
    
    for(int i = 51; i > 0; i--) {
        int j = rand() % (i + 1);
        Carta temp = mazo.mazo[i];
        mazo.mazo[i] = mazo.mazo[j];
        mazo.mazo[j] = temp;
    }
}

void inicializarMano(Mano* mano) {
    mano->numCartas = 0;
    mano->total = 0;
    mano->ases = 0;
}

int obtenerValorCarta(int valor) {
    // AS = 1 (se ajusta a 11 en calcularTotal si conviene)
    // 2-10 = valor nominal
    // J, Q, K (11, 12, 13) = 10 puntos
    if(valor >= 11 && valor <= 13) return 10; // J, Q, K valen 10
    return valor;
}

void calcularTotal(Mano* mano) {
    mano->total = 0;
    mano->ases = 0;
    
    for(int i = 0; i < mano->numCartas; i++) {
        if(!mano->cartas[i].visible) continue;
        
        int valor = obtenerValorCarta(mano->cartas[i].valor);
        mano->total += valor;
        
        if(mano->cartas[i].valor == 1) {
            mano->ases++;
        }
    }
    
    int asesUsables = mano->ases;
    while(asesUsables > 0 && mano->total + 10 <= 21) {
        mano->total += 10;
        asesUsables--;
    }
}

void repartirCarta(Mano* mano, int visible) {
    if(mazo.cartasRestantes > 0 && mano->numCartas < 12) {
        mano->cartas[mano->numCartas] = mazo.mazo[52 - mazo.cartasRestantes];
        mano->cartas[mano->numCartas].visible = visible;
        mano->numCartas++;
        mazo.cartasRestantes--;
        calcularTotal(mano);
    }
}

int verificarBlackjack(Mano* mano) {
    return (mano->numCartas == 2 && mano->total == 21);
}

int verificarBust(Mano* mano) {
    return (mano->total > 21);
}

void revelarCartaDealer() {
    for(int i = 0; i < dealer.numCartas; i++) {
        dealer.cartas[i].visible = 1;
    }
    calcularTotal(&dealer);
}

void jugarDealer() {
    revelarCartaDealer();
    
    while(dealer.total < 17) {
        repartirCarta(&dealer, 1);
    }
}

void determinarGanador() {
    int bjJugador = verificarBlackjack(&jugador);
    int bjDealer = verificarBlackjack(&dealer);
    
    ganancias = 0; // Reset ganancias
    
    if(bjJugador && bjDealer) {
        // Empate - devolver apuesta
        strcpy(mensajeResultado, "EMPATE BLACKJACK!");
        ganancias = 0;
    } else if(bjJugador) {
        // Blackjack paga 3:2 (1.5x la apuesta)
        strcpy(mensajeResultado, "BLACKJACK!");
        ganancias = (apuestaActual * 3) / 2;
        balanceJugador += apuestaActual + ganancias;
        balanceCasa -= ganancias;
    } else if(bjDealer) {
        // Dealer gana - pierde apuesta
        strcpy(mensajeResultado, "Dealer Blackjack.");
        ganancias = -apuestaActual;
        balanceJugador -= apuestaActual;
        balanceCasa += apuestaActual;
    } else if(verificarBust(&jugador)) {
        // Jugador se pasa - pierde apuesta
        strcpy(mensajeResultado, "TE PASASTE!");
        ganancias = -apuestaActual;
        balanceJugador -= apuestaActual;
        balanceCasa += apuestaActual;
    } else if(verificarBust(&dealer)) {
        // Dealer se pasa - gana apuesta 1:1
        strcpy(mensajeResultado, "DEALER SE PASO!");
        ganancias = apuestaActual;
        balanceJugador += apuestaActual + ganancias;
        balanceCasa -= ganancias;
    } else if(jugador.total > dealer.total) {
        // Jugador gana por puntos - 1:1
        strcpy(mensajeResultado, "GANASTE!");
        ganancias = apuestaActual;
        balanceJugador += apuestaActual + ganancias;
        balanceCasa -= ganancias;
    } else if(jugador.total < dealer.total) {
        // Dealer gana - pierde apuesta
        strcpy(mensajeResultado, "PERDISTE.");
        ganancias = -apuestaActual;
        balanceJugador -= apuestaActual;
        balanceCasa += apuestaActual;
    } else {
        // Empate - devolver apuesta
        strcpy(mensajeResultado, "EMPATE!");
        ganancias = 0;
    }
}

// --- FUNCIONES DE RENDERIZADO ---
const char* obtenerTextoCartaGrande(int valor) {
    static char texto[4];
    // Mostrar nombres especiales para visualización
    // Internamente J=11, Q=12, K=13, pero valen 10 puntos
    switch(valor) {
        case 1:  return "AS";
        case 11: return "J";
        case 12: return "Q";
        case 13: return "K";
        default: 
            sprintf(texto, "%d", valor);
            return texto;
    }
}

unsigned int obtenerColorCarta(int palo) {
    return (palo == 0 || palo == 1) ? colorRojo : colorNegro;
}

void dibujarCarta(float x, float y, Carta* carta, int oculta) {
    float ancho = 50;
    float alto = 70;
    
    if(oculta || !carta->visible) {
        // Carta volteada - Rosa
        drawRect(x, y, ancho, alto, colorBorde);
        drawRect(x + 2, y + 2, ancho - 4, alto - 4, colorCartaReverso);
        // Patrón decorativo
        drawRect(x + 10, y + alto/2 - 2, ancho - 20, 4, 0xFFFFD4E5);
    } else {
        // Carta frontal - Blanca
        drawRect(x, y, ancho, alto, colorBorde);
        drawRect(x + 2, y + 2, ancho - 4, alto - 4, colorBlanco);
        
        // Indicador de color (más grande que antes)
        unsigned int colorIndicador = obtenerColorCarta(carta->palo);
        drawRect(x + ancho/2 - 8, y + alto/2 - 8, 16, 16, colorIndicador);
    }
}

void dibujarMano(float startX, float y, Mano* mano, int ocultarPrimera) {
    for(int i = 0; i < mano->numCartas; i++) {
        int ocultar = (i == 0 && ocultarPrimera);
        dibujarCarta(startX + (i * 55), y, &mano->cartas[i], ocultar);
    }
}

void renderizarTexto() {
    pspDebugScreenSetXY(0, 0);
    
    switch(estadoActual) {
        case ESTADO_MENU:
            pspDebugScreenSetXY(25, 10);
            pspDebugScreenPrintf("=== BLACKJACK ===");
            pspDebugScreenSetXY(25, 12);
            pspDebugScreenPrintf("Balance: $%d", balanceJugador);
            pspDebugScreenSetXY(25, 13);
            pspDebugScreenPrintf("Casa: $%d", balanceCasa);
            pspDebugScreenSetXY(25, 15);
            pspDebugScreenPrintf("Presiona X para jugar");
            pspDebugScreenSetXY(25, 16);
            pspDebugScreenPrintf("Presiona START para salir");
            
            // Verificar condiciones de victoria/derrota
            if(balanceJugador < apuestaMinima) {
                pspDebugScreenSetXY(20, 20);
                pspDebugScreenPrintf("SIN CREDITOS SUFICIENTES!");
            }
            if(balanceCasa <= 0) {
                pspDebugScreenSetXY(20, 20);
                pspDebugScreenPrintf("GANASTE TODO! DERROTASTE A LA CASA!");
            }
            break;
            
        case ESTADO_APUESTA:
            pspDebugScreenSetXY(2, 10);
            pspDebugScreenPrintf("=== HACER APUESTA ===");
            pspDebugScreenSetXY(2, 12);
            pspDebugScreenPrintf("Balance disponible: $%d", balanceJugador);
            pspDebugScreenSetXY(2, 14);
            pspDebugScreenPrintf("Apuesta actual: $%d", apuestaActual);
            pspDebugScreenSetXY(2, 15);
            pspDebugScreenPrintf("(Minimo: $%d)", apuestaMinima);
            
            pspDebugScreenSetXY(2, 18);
            pspDebugScreenPrintf("ARRIBA/ABAJO = Ajustar apuesta");
            pspDebugScreenSetXY(2, 19);
            pspDebugScreenPrintf("X = Confirmar y jugar");
            pspDebugScreenSetXY(2, 20);
            pspDebugScreenPrintf("O = Cancelar");
            break;
            
        case ESTADO_REPARTIENDO:
            pspDebugScreenSetXY(25, 1);
            pspDebugScreenPrintf("Repartiendo cartas...");
            break;
            
        case ESTADO_JUGADOR:
        case ESTADO_DEALER:
            // Dealer
            pspDebugScreenSetXY(2, 5);
            pspDebugScreenPrintf("DEALER");
            if(estadoActual == ESTADO_DEALER) {
                pspDebugScreenSetXY(2, 6);
                pspDebugScreenPrintf("Total: %d", dealer.total);
            } else {
                pspDebugScreenSetXY(2, 6);
                pspDebugScreenPrintf("Total: ?");
            }
            
            // Cartas del dealer
            for(int i = 0; i < dealer.numCartas; i++) {
                pspDebugScreenSetXY(2, 7 + i);
                if(i == 0 && estadoActual == ESTADO_JUGADOR) {
                    pspDebugScreenPrintf("Carta %d: ??", i + 1);
                } else {
                    pspDebugScreenPrintf("Carta %d: %s", i + 1, 
                        obtenerTextoCartaGrande(dealer.cartas[i].valor));
                }
            }
            
            // Jugador
            pspDebugScreenSetXY(2, 19);
            pspDebugScreenPrintf("JUGADOR");
            pspDebugScreenSetXY(2, 20);
            pspDebugScreenPrintf("Total: %d", jugador.total);
            
            // Cartas del jugador
            for(int i = 0; i < jugador.numCartas; i++) {
                pspDebugScreenSetXY(2, 21 + i);
                pspDebugScreenPrintf("Carta %d: %s", i + 1,
                    obtenerTextoCartaGrande(jugador.cartas[i].valor));
            }
            
            // Información de apuesta y balance
            pspDebugScreenSetXY(2, 31);
            pspDebugScreenPrintf("Apuesta: $%d", apuestaActual);
            pspDebugScreenSetXY(2, 32);
            pspDebugScreenPrintf("Balance: $%d", balanceJugador);
            
            // Controles
            if(estadoActual == ESTADO_JUGADOR) {
                pspDebugScreenSetXY(20, 31);
                pspDebugScreenPrintf("X = Pedir | O = Plantarse");
            } else if(estadoActual == ESTADO_DEALER) {
                pspDebugScreenSetXY(20, 15);
                pspDebugScreenPrintf("Dealer jugando...");
            }
            break;
            
        case ESTADO_RESULTADO:
            // Dealer final
            pspDebugScreenSetXY(2, 5);
            pspDebugScreenPrintf("DEALER - Total: %d", dealer.total);
            for(int i = 0; i < dealer.numCartas; i++) {
                pspDebugScreenSetXY(2, 6 + i);
                pspDebugScreenPrintf("Carta %d: %s", i + 1,
                    obtenerTextoCartaGrande(dealer.cartas[i].valor));
            }
            
            // Jugador final
            pspDebugScreenSetXY(2, 19);
            pspDebugScreenPrintf("JUGADOR - Total: %d", jugador.total);
            for(int i = 0; i < jugador.numCartas; i++) {
                pspDebugScreenSetXY(2, 20 + i);
                pspDebugScreenPrintf("Carta %d: %s", i + 1,
                    obtenerTextoCartaGrande(jugador.cartas[i].valor));
            }
            
            // Resultado - centrado
            pspDebugScreenSetXY(25, 15);
            pspDebugScreenPrintf(">>> %s <<<", mensajeResultado);
            
            // Mostrar ganancias/pérdidas
            pspDebugScreenSetXY(25, 16);
            if(ganancias > 0) {
                pspDebugScreenPrintf("Ganaste: +$%d", ganancias);
            } else if(ganancias < 0) {
                pspDebugScreenPrintf("Perdiste: $%d", -ganancias);
            } else {
                pspDebugScreenPrintf("Empate - Apuesta devuelta");
            }
            
            // Balance actualizado
            pspDebugScreenSetXY(25, 17);
            pspDebugScreenPrintf("Balance: $%d", balanceJugador);
            pspDebugScreenSetXY(25, 18);
            pspDebugScreenPrintf("Casa: $%d", balanceCasa);
            
            pspDebugScreenSetXY(10, 31);
            pspDebugScreenPrintf("Presiona TRIANGULO = Nueva partida misma apuesta");
            pspDebugScreenSetXY(10, 32);
            pspDebugScreenPrintf("Presiona START = Volver al menu para cambiar apuesta");
            break;
    }
}

// --- LÓGICA DE ESTADOS ---
void iniciarNuevaPartida() {
    inicializarMazo();
    inicializarMano(&jugador);
    inicializarMano(&dealer);
    
    repartirCarta(&jugador, 1);
    repartirCarta(&jugador, 1);
    repartirCarta(&dealer, 0);
    repartirCarta(&dealer, 1);
    
    estadoActual = ESTADO_REPARTIENDO;
    frameDelay = 60;
}

void actualizarJuego(SceCtrlData* pad) {
    static SceCtrlData oldPad;
    int pressed = pad->Buttons & ~oldPad.Buttons;
    
    if(frameDelay > 0) {
        frameDelay--;
        oldPad = *pad;
        return;
    }
    
    switch(estadoActual) {
        case ESTADO_MENU:
            if(pressed & PSP_CTRL_CROSS) {
                // Verificar si tiene créditos suficientes
                if(balanceJugador >= apuestaMinima && balanceCasa > 0) {
                    apuestaActual = apuestaMinima; // Reset apuesta a mínimo
                    estadoActual = ESTADO_APUESTA;
                }
            }
            if(pressed & PSP_CTRL_START) running = 0;
            break;
            
        case ESTADO_APUESTA:
            // Ajustar apuesta con pad arriba/abajo
            if(pressed & PSP_CTRL_UP) {
                if(apuestaActual + 50 <= balanceJugador) {
                    apuestaActual += 50;
                }
            }
            if(pressed & PSP_CTRL_DOWN) {
                if(apuestaActual - 50 >= apuestaMinima) {
                    apuestaActual -= 50;
                }
            }
            // Confirmar apuesta y empezar juego
            if(pressed & PSP_CTRL_CROSS) {
                iniciarNuevaPartida();
            }
            // Cancelar y volver al menú
            if(pressed & PSP_CTRL_CIRCLE) {
                estadoActual = ESTADO_MENU;
            }
            break;
            
        case ESTADO_REPARTIENDO:
            if(verificarBlackjack(&jugador) || verificarBlackjack(&dealer)) {
                revelarCartaDealer();
                determinarGanador();
                estadoActual = ESTADO_RESULTADO;
            } else {
                estadoActual = ESTADO_JUGADOR;
            }
            break;
            
        case ESTADO_JUGADOR:
            if(pressed & PSP_CTRL_CROSS) {
                repartirCarta(&jugador, 1);
                if(verificarBust(&jugador)) {
                    revelarCartaDealer();
                    determinarGanador();
                    estadoActual = ESTADO_RESULTADO;
                } else if(jugador.total == 21) {
                    estadoActual = ESTADO_DEALER;
                    frameDelay = 30;
                }
            }
            if(pressed & PSP_CTRL_CIRCLE) {
                estadoActual = ESTADO_DEALER;
                frameDelay = 30;
            }
            break;
            
        case ESTADO_DEALER:
            jugarDealer();
            determinarGanador();
            estadoActual = ESTADO_RESULTADO;
            break;
            
        case ESTADO_RESULTADO:
            if(pressed & PSP_CTRL_TRIANGLE) iniciarNuevaPartida();
            if(pressed & PSP_CTRL_START) estadoActual = ESTADO_MENU;
            break;
    }
    
    oldPad = *pad;
}

// --- MAIN ---
int main() {
    setup_callbacks();
    srand(time(NULL));
    initGu();

    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    while(running) {
        sceCtrlReadBufferPositive(&pad, 1);
        actualizarJuego(&pad);

        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(bgColor);
        sceGuClear(GU_COLOR_BUFFER_BIT);

        if(estadoActual != ESTADO_MENU) {
            int ocultarDealer = (estadoActual == ESTADO_JUGADOR || estadoActual == ESTADO_REPARTIENDO);
            
            float anchoTotalDealer = (dealer.numCartas > 0) ? (dealer.numCartas - 1) * 55 + 50 : 50;
            float startXDealer = (SCREEN_WIDTH - anchoTotalDealer) / 2;
            
            float anchoTotalJugador = (jugador.numCartas > 0) ? (jugador.numCartas - 1) * 55 + 50 : 50;
            float startXJugador = (SCREEN_WIDTH - anchoTotalJugador) / 2;
            
            dibujarMano(startXDealer, 45, &dealer, ocultarDealer);
            dibujarMano(startXJugador, 165, &jugador, 0);
        }

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        
        // Renderizar texto después del swap
        renderizarTexto();
    }

    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
