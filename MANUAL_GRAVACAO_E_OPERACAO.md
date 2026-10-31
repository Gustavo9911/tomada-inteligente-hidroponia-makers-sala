# Manual da etapa 2 — gravação e operação

Este guia acompanha a gravação da ESP32 e o primeiro teste da Tomada Inteligente para Hidroponia, desenvolvida pela equipe Makers Sala, da Escola Estadual Adib Miguel Haddad. **Não conecte a bomba à rede elétrica durante a gravação ou os primeiros testes.** A placa, o relé e o circuito da bomba precisam estar montados e protegidos por pessoa habilitada.

## 1. Arquivos que precisam ficar juntos

Na mesma pasta do sketch devem estar:

- `TomadaHidroponia.ino` — firmware da placa.
- `HydroSecrets.h` — arquivo privado com Wi-Fi, credenciais locais, endereço do serviço remoto, token do dispositivo e certificado TLS.

`HydroSecrets.h` é excluído do Git de propósito. Não o publique no GitHub nem compartilhe foto, cópia ou captura que revele o conteúdo. Se ele não estiver junto do `.ino`, o sketch não compilará. O código-fonte não deve conter as chaves secretas da Vercel/Supabase.

## 2. Instalar o Arduino IDE e dependências

1. Instale o Arduino IDE 2.x.
2. Em **Arquivo → Preferências → URLs adicionais para Gerenciadores de Placas**, adicione a URL oficial do pacote ESP32 da Espressif: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
3. Abra **Ferramentas → Placa → Gerenciador de placas**, procure **esp32 by Espressif Systems** e instale uma versão estável compatível com o sketch. BLE integrado usa as bibliotecas do próprio core ESP32.
4. Em **Gerenciador de Bibliotecas**, instale:
   - **DHT sensor library** (Adafruit);
   - **Adafruit Unified Sensor**;
   - **ArduinoJson** (compatível com 6.x/7.x).
5. `WiFi`, `WebServer`, `Preferences`, `time`, `HTTPClient`, `WiFiClientSecure` e BLE são fornecidos pelo core ESP32.

## 3. Montagem de baixa tensão

| Módulo | Conexão ESP32 | Alimentação / observação |
|---|---|---|
| Entrada `IN` do relé | GPIO 23 | Relé ativo em LOW no firmware; módulo compatível com lógica de 3,3 V. |
| DHT22 `DATA` | GPIO 4 | VCC em 3,3 V e GND comum. DHT22 sem placa pode precisar de resistor pull-up 4,7–10 kΩ entre DATA e 3,3 V. |
| Chave boia | GPIO 5 e GND | `INPUT_PULLUP`; no firmware LOW representa nível baixo. Verifique a orientação/contato da sua boia. |
| Buzzer ativo | GPIO 18 e GND | Pulso intermitente sem `delay()`. Use transistor se o buzzer exceder a corrente segura do GPIO. |
| Módulo relé | `VCC` 5 V, `GND` e `IN` GPIO 23 | A fonte USB 5 V/2 A alimenta ESP32 e módulo. Confirme a corrente e a compatibilidade da placa. |
| ESP32 DevKit WROOM-32 | Alimentação pela USB | DHT22 e GPIOs usam lógica 3,3 V. Não aplique 5 V aos GPIOs. |

GND da ESP32, do DHT22, do buzzer e do módulo de relé deve ser comum quando o módulo não fornecer isolamento apropriado. A boia fecha para GND para indicar nível baixo. Faça primeiro o teste da boia desconectado da bomba.

### Parte de 127/220 V

A carga da bomba deve passar pelos contatos `COM` e `NO` do relé, de forma que a bomba fique desligada quando o relé estiver desenergizado. **Não monte tensão de rede em protoboard.** A fiação de 127/220 V deve ficar dentro de caixa apropriada, com isolamento, fusível, DR e alívio de tração, feita e revisada por pessoa habilitada. Mantenha rede elétrica e água fisicamente separadas. Para apresentação, prefira demonstrar primeiro com uma carga de baixa tensão; a ligação da bomba à rede só deve ser energizada após inspeção profissional.

## 4. Gravar o firmware

1. Faça uma cópia de segurança dos dois arquivos (`.ino` e `HydroSecrets.h`).
2. Abra `TomadaHidroponia.ino` no Arduino IDE. Confirme que o arquivo privado foi localizado na mesma pasta.
3. Em **Ferramentas → Placa**, escolha **ESP32 Dev Module**. Em **Partition Scheme/Esquema de partição**, selecione **Huge APP (3MB No OTA/1MB SPIFFS)**. O firmware completo ocupa cerca de 1,88 MB e não cabe na partição padrão de 1,2 MB; a opção Huge APP é necessária. Ela reserva espaço maior para o programa e não oferece atualização OTA (a gravação é por USB). Conecte a placa com cabo USB de dados e selecione a porta serial correspondente.
4. Instale as bibliotecas indicadas na seção 2. Clique em **Verificar** para compilar.
5. Com a bomba desenergizada, clique em **Carregar**. Se aparecer `Connecting...`, pressione e mantenha `BOOT` até a gravação começar; solte depois.
6. Abra o **Monitor Serial** em `115200 baud` e pressione `EN/RESET`. Não compartilhe registros que possam expor dados privados.
7. A placa inicia a agenda local, sensores, painel, BLE e tentativa de conexão Wi-Fi. A sincronização NTP usa horário de Brasília (BRT, UTC−3). Sem internet, o relógio não fica sincronizado e a agenda por hora não pode ser considerada confiável até sincronizar.

## 5. Acesso por Wi-Fi

### Rede local configurada

O firmware tenta conectar à rede 2,4 GHz indicada no `HydroSecrets.h`. Consulte o Monitor Serial para descobrir o IP concedido pelo roteador e abra `http://IP-DA-ESP32/` no navegador do celular/computador conectado à mesma rede. A interface local exige login e sessão. As credenciais são as configuradas no arquivo privado; não as publique em documentação pública.

### Ponto de acesso de emergência

Se a estação não conectar em até 30 segundos, o firmware inicia o AP `Hidroponia-ESP32`, protegido pela senha privada `AP_PASSWORD` definida em `HydroSecrets.h`, no endereço `192.168.4.1`. Consulte esse arquivo local para descobrir a senha. Conecte o celular a essa rede e abra `http://192.168.4.1/`. O AP serve para acesso local quando o roteador está ausente; ele não fornece internet. A agenda e a segurança por nível continuam no próprio microcontrolador.

## 6. Acesso por Bluetooth LE

O dispositivo anuncia o nome `Hidroponia-ESP32`. No painel remoto servido em HTTPS, use **Conectar por Bluetooth** em um navegador compatível (por exemplo, Chrome/Edge em aparelho com suporte a Web Bluetooth). Aceite a seleção do dispositivo. Algumas combinações de iOS, navegador e sistema operacional não disponibilizam Web Bluetooth; nesse caso use Android compatível ou um cliente BLE como nRF Connect.

O BLE é comunicação local de curto alcance, independente da conexão com a nuvem. O serviço GATT e os UUIDs, permissões e formato JSON estão documentados na seção **BLE GATT** do `README.md`. Comandos BLE passam pelo mesmo controle de nível de água; nível baixo bloqueia a partida.

## 7. Como o Vercel se vincula à placa

O painel publicado está em [adib-hidroponia-mika-projects1.vercel.app](https://adib-hidroponia-mika-projects1.vercel.app). A integração já tem backend Supabase e variáveis server-side cadastrados. O firmware inicia conexões HTTPS de saída até a API Vercel; não é necessário abrir portas no roteador nem configurar encaminhamento de portas.

1. Confira no `HydroSecrets.h` o Wi-Fi com acesso à internet, `CLOUD_BASE_URL`, o ID do dispositivo e o token exclusivo já provisionado para a ESP32. Não altere o token em um só lado: o valor do firmware precisa corresponder à variável `DEVICE_TOKEN` configurada na Vercel.
2. Mantenha a CA raiz TLS presente no arquivo. Não use `setInsecure()` nem desative a validação HTTPS.
3. Grave o sketch e aguarde a conexão Wi-Fi e a primeira telemetria. O painel remoto pode mostrar o dispositivo offline até receber essa primeira atualização.
4. Faça login no painel remoto com o usuário e a senha cadastrados no backend. O painel e as APIs exigem sessão; as chaves administrativas Supabase permanecem somente na Vercel.
5. Confirme que estado, sensores e horário aparecem com dados reais. Comande `DESLIGAR` primeiro e espere a confirmação recebida/executada. Depois teste `LIGAR` apenas com nível normal e uma carga de baixa tensão.

O ESP32 envia telemetria e consulta comandos pela internet. A nuvem não energiza o relé diretamente: a placa valida os comandos, aplica a agenda e faz a última verificação de nível. Se Wi-Fi, internet, Vercel ou Supabase falhar, o controle remoto pode parar, mas a agenda e o bloqueio de nível no firmware continuam funcionando localmente. O histórico mantido no firmware é volátil entre reinicializações, a menos que uma funcionalidade explícita persista os eventos; os registros enviados à nuvem dependem de telemetria recebida.

## 8. Roteiro de teste antes da bomba

1. Sem carga de rede, ligue a ESP32 e confirme que o relé inicia no estado desligado.
2. Abra a interface local e faça login. Confirme que chamadas sem sessão são recusadas.
3. Verifique leituras DHT22; erro de leitura deve aparecer como indisponível, não como número inventado.
4. Mude a boia para indicar nível baixo. O buzzer deve piscar e qualquer pedido de ligar deve ser bloqueado; o relé deve desligar.
5. Retorne a boia ao nível normal e confira se o status só muda após o debounce.
6. Cadastre uma janela de horário próxima e confirme que ela permanece após reiniciar a ESP32. Feche o navegador e observe a automação continuar.
7. Teste `LIGAR`, `DESLIGAR` e `AUTOMÁTICO`; o modo manual deve expirar depois de 30 minutos.
8. Na seção de horários, toque em **Adicionar horário**, informe **Ligar** e **Desligar**, deixe **Ativo** marcado e salve. Adicione/desative/remova linhas e confirme o limite de oito; após reiniciar, a programação gravada deve continuar. Pela nuvem, confirme na tela que a agenda foi aplicada pela ESP32.
9. Teste o BLE e confirme o retorno do estado/acknowledgment.
10. Conecte a rede com internet e confira o indicador online no painel Vercel após telemetria real. Envie primeiro `DESLIGAR` e confirme o retorno da placa.
11. Desconecte o Wi-Fi e confirme que o controle local continua ativo; o remoto deve indicar dado desatualizado/offline.
12. Após a revisão da caixa e da instalação por profissional habilitado, faça o teste final da bomba em água e sob supervisão. Nunca deixe a bomba operar fora da água se o modelo não permitir.

## 9. Solução de problemas

| Sintoma | Verificação |
|---|---|
| `HydroSecrets.h: No such file` | Coloque o arquivo privado ao lado do `.ino`; não o copie para o Git. |
| ESP32 não grava | Use cabo USB de dados, confirme placa/porta e segure `BOOT` durante `Connecting...`. |
| Não conecta ao roteador | Confirme SSID/senha e rede 2,4 GHz no arquivo privado; AP aparece após 30 s. |
| Painel remoto mostra offline | Verifique Wi-Fi com internet, hora/CA TLS, token pareado nos dois lados e recepção de telemetria. Não publique token em logs. |
| DHT22 indisponível | Confira DATA no GPIO 4, VCC 3,3 V, GND e pull-up; aguarde a amostragem de 2,5 s. |
| Relé liga ao contrário | Confirme que o módulo é compatível com lógica ativa em LOW usada no firmware e teste sem bomba. |
| Bomba não liga | Verifique estado da boia, agenda/modo manual, indicação de nível baixo, fonte e fiação do módulo; proteção de nível sempre prevalece. |
| Bluetooth não aparece no site | Use HTTPS e aparelho/navegador que ofereça Web Bluetooth; confirme que o dispositivo está ligado e anunciando. |

## 10. Critérios para a demonstração científica

Registre versão do firmware, data/hora, condições do cultivo, configuração de cada horário, eventos da boia e leituras. Para comparar estratégias, mantenha iguais planta, solução nutritiva, volume do reservatório e iluminação. Meça energia com wattímetro apropriado, e não estime apenas pelos 5 W nominais da bomba. Compare funcionamento manual, agenda sem bloqueio e agenda com proteção; reporte repetições, médias, variação, falhas e limitações. Não afirme aumento de produtividade sem um experimento replicado que o demonstre.

### Justificativa científica curta

O cultivo hidropônico depende da circulação de solução nutritiva e da manutenção de condições adequadas ao desenvolvimento vegetal. Este projeto investiga uma automação acessível que programa o acionamento da bomba, monitora temperatura e umidade e bloqueia a operação quando o nível do reservatório é insuficiente. O registro de ciclos, duração, ocorrências e consumo permite comparar estratégias de manejo com dados observáveis e avaliar limites de segurança e eficiência de um sistema IoT aplicado à hidroponia.

## Crédito do projeto

**Equipe Projeto Maker Sala,**

**Orientador/coorientadora;**
-
-
---------------------------------
**Alunos;**
-
-
-
**— Escola Estadual Adib Miguel Haddad.** **Projeto de automação e hidroponia.**
