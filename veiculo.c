#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

volatile sig_atomic_t viagem_cancelada = 0;

void handle_sigusr1(int sig) {
    (void)sig; // Evita o aviso de "unused parameter"
    viagem_cancelada = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: ./veiculo <distancia> <fifo_cliente> <id_viagem>\n");
        return 1;
    }

    int distancia = atoi(argv[1]);
    char *fifo_cliente_nome = argv[2];
    int id_viagem = atoi(argv[3]);

    signal(SIGUSR1, handle_sigusr1);

    printf("%d INICIOU\n", id_viagem);
    fflush(stdout);

    // Tenta abrir o FIFO em modo NONBLOCK para não bloquear se o cliente não estiver lá
    int fd_fifo_cliente = open(fifo_cliente_nome, O_WRONLY | O_NONBLOCK);

    // Se falhar com ENXIO, significa que ninguém abriu para leitura (cliente offline)
    if (fd_fifo_cliente == -1) {
        if (errno == ENXIO) {
            fprintf(stderr, "Veiculo %d: Cliente offline (FIFO nao aberto para leitura)\n", id_viagem);
            printf("%d ERRO Cliente offline\n", id_viagem);
        } else {
            perror("Veiculo: Erro ao abrir FIFO");
            printf("%d ERRO Falha no FIFO\n", id_viagem);
        }
        fflush(stdout);
        return 1;
    }

    // Se abriu com sucesso, enviamos a mensagem
    char msg_chegada[200];
    snprintf(msg_chegada, sizeof(msg_chegada), "O seu veiculo para a viagem %d chegou. A viagem vai comecar.", id_viagem);
    write(fd_fifo_cliente, msg_chegada, strlen(msg_chegada));
    close(fd_fifo_cliente);

    printf("%d CLIENTE_ENTROU\n", id_viagem);
    fflush(stdout);

    int distancia_percorrida = 0;
    int progresso_reportado = 0;

    for (distancia_percorrida = 1; distancia_percorrida <= distancia; distancia_percorrida++) {
        sleep(1);

        if (viagem_cancelada) {
            printf("%d CANCELADA\n", id_viagem);
            fflush(stdout);

            // Tenta avisar o cliente
            fd_fifo_cliente = open(fifo_cliente_nome, O_WRONLY | O_NONBLOCK);
            if (fd_fifo_cliente != -1) {
                write(fd_fifo_cliente, "A sua viagem foi cancelada.", 27);
                close(fd_fifo_cliente);
            }
            return 0;
        }

        int progresso_atual = (int)(((float)distancia_percorrida / distancia) * 100);
        if (progresso_atual >= progresso_reportado + 10) {
            progresso_reportado = (progresso_atual / 10) * 10;
            printf("%d PROGRESSO %d\n", id_viagem, progresso_reportado);
            fflush(stdout);
        }
    }

    printf("%d CONCLUIDA\n", id_viagem);
    fflush(stdout);

    fd_fifo_cliente = open(fifo_cliente_nome, O_WRONLY | O_NONBLOCK);
    if (fd_fifo_cliente != -1) {
        write(fd_fifo_cliente, "Chegou ao seu destino. Obrigado por viajar connosco!", 51);
        close(fd_fifo_cliente);
    }

    return 0;
}
