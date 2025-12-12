#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#define FIFO_PRINCIPAL "controlador_fifo"
#define FIFO_CLIENTE_TEMPLATE "cliente_%s_fifo"

char fifo_cliente_nome[100];

// Função de limpeza chamada à saída para remover o FIFO
void cleanup_cliente() {
    printf("\nCliente a terminar... a limpar o FIFO %s\n", fifo_cliente_nome);
    unlink(fifo_cliente_nome);
}

// Handler para o sinal SIGINT (Ctrl+C) para garantir a limpeza
void handle_sigint_cliente(int sig) {
    (void)sig; // Evita aviso de 'unused parameter'
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <username>\n", argv[0]);
        return 1;
    }

    char *username = argv[1];
    int fd_fifo_principal, fd_fifo_cliente;
    pid_t pid;

    printf("Cliente '%s' a iniciar...\n", username);

    fd_fifo_principal = open(FIFO_PRINCIPAL, O_WRONLY);
    if (fd_fifo_principal == -1) {
        perror("Erro ao abrir FIFO do controlador. O controlador está a correr?");
        return 1;
    }
    printf("Ligado ao controlador.\n");

    snprintf(fifo_cliente_nome, sizeof(fifo_cliente_nome), FIFO_CLIENTE_TEMPLATE, username);
    atexit(cleanup_cliente);
    signal(SIGINT, handle_sigint_cliente);

    unlink(fifo_cliente_nome); // Limpa FIFO antigo
    if (mkfifo(fifo_cliente_nome, 0666) == -1) {
        perror("Erro ao criar o FIFO do cliente");
        return 1;
    }
    printf("FIFO do cliente '%s' criado.\n", fifo_cliente_nome);

    char mensagem_login[256];
    snprintf(mensagem_login, sizeof(mensagem_login), "LOGIN %s %s", username, fifo_cliente_nome);
    write(fd_fifo_principal, mensagem_login, strlen(mensagem_login));
    printf("Mensagem de login enviada.\n");

    pid = fork();
    if (pid == -1) {
        perror("Erro no fork");
        return 1;
    }

    if (pid == 0) { // Filho: Recebe mensagens
        signal(SIGTERM, handle_sigint_cliente); // Usa o mesmo handler para terminação limpa
        fd_fifo_cliente = open(fifo_cliente_nome, O_RDONLY);
        if (fd_fifo_cliente == -1) {
            perror("Filho: Erro ao abrir FIFO do cliente"); exit(1);
        }
        char buffer_recebido[512];
        int n;
        printf("\n[INFO] Estou à escuta de mensagens.\n");
        while ((n = read(fd_fifo_cliente, buffer_recebido, sizeof(buffer_recebido))) > 0) {
            buffer_recebido[n] = '\0';
            printf("\n[MENSAGEM] %s\n> ", buffer_recebido);
            fflush(stdout);
        }
        close(fd_fifo_cliente);
        exit(0);
    } else { // Pai: Envia comandos
        char comando[256];
        printf("Introduza os seus comandos (ex: 'agendar 100 lisboa 50', 'terminar').\n");
        while (1) {
            printf("> ");
            if (fgets(comando, sizeof(comando), stdin) == NULL) break;
            comando[strcspn(comando, "\n")] = 0;

            if (strcmp(comando, "help") == 0) {
                printf("--- Comandos do Cliente ---\n");
                printf("  agendar <hora> <local> <distancia> - Agenda um servico.\n");
                printf("  consultar                      - Mostra os seus servicos agendados.\n");
                printf("  cancelar <id>                  - Cancela um servico agendado (se id for 0, cancela todos).\n");
                printf("  terminar                       - Sai da aplicacao cliente.\n");
                printf("  help                           - Mostra esta ajuda.\n");
            } else if (strcmp(comando, "terminar") == 0) {
                char msg_terminar[300];
                snprintf(msg_terminar, sizeof(msg_terminar), "%s terminar", username);
                write(fd_fifo_principal, msg_terminar, strlen(msg_terminar));
                break;
            } else {
                char mensagem_comando[512];
                snprintf(mensagem_comando, sizeof(mensagem_comando), "%s %s", username, comando);
                write(fd_fifo_principal, mensagem_comando, strlen(mensagem_comando));
            }
        }
        kill(pid, SIGTERM);
        wait(NULL);
        close(fd_fifo_principal);
        printf("A terminar a sessão...\n");
    }
    return 0;
}
