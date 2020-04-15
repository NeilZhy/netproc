
#include <stdio.h>
#include "process.h"


static void cls(void);

void
show_process(const process_t *const processes, const size_t tot_process)
{
  cls();

  printf("%-5s\t %-45s %s\t %s\t %-14s\t %s \n",
        "PID", "PROGRAM", "PPS TX", "PPS RX", "RATE UP", "RATE DOWN");

  for (size_t i = 0; i < tot_process; i++)
    {
      printf("%-5d\t %-45s %ld\t %ld\t %-14s\t %s\t \n",
            processes[i].pid,
            processes[i].name,
            processes[i].net_stat.avg_pps_tx,
            processes[i].net_stat.avg_pps_rx,
            processes[i].net_stat.tx_rate,
            processes[i].net_stat.rx_rate);
    }
}

// limpa tela
static void cls(void){
   printf("\033[2J");   // Limpa a tela
   printf("\033[0;0H"); // Devolve o cursor para a linha 0, coluna 0

//https://pt.stackoverflow.com/questions/58453/como-fazer-efeito-de-loading-no-terminal-em-apenas-uma-linha
}
