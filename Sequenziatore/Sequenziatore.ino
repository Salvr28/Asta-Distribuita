#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <vector>
#include <map>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>           
#include <LiquidCrystal_I2C.h>

#define DURATION_TIME 60000
#define NUM_NODES 5
#define BUTTON_AUCTION_PIN 18
#define BUTTON_BID_PIN 19
#define SDA_PIN 21
#define SCL_PIN 22
#define LCD_I2C_ADDRESS 0x27

/*
############### LEGGERE PER CONFIGURAZIONE #########################
Essendo un prototipo del prodotto finale bisogna configurare "a mano" l'ambiente di rete del sistema:
- Configurare il nome della rete WiFi e la password
- Configurare l'indirizzo IP del server che riceve i dati
- Configurare l'indirizzo MAC del sequenziatore
- Configurare l'indirizzo MAC dei nodi partecipanti
- Cambiare manualmente il canale wifi delle schede ed uniformarlo
#####################################################################
*/

// Configurazione WiFi
const char* ssid = "POCO F3";
const char* password = "280901sal";
const char* serverUrl = "http://192.168.208.157:8000/receive-data";

// Crea un'istanza dell'oggetto LiquidCrystal_I2C
// (Indirizzo I2C, Numero colonne, Numero righe)
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);

// Indirizzo di broadcast per inviare a tutti i nodi
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

//Indirizzo mac della scheda scelta come sequenziatore
String mac_sequencer = "F8:B3:B7:2C:71:80";

// Crea la mappa per associare MAC address a numeri da 0 a 4
std::map<String, int> macToNumberMap;

// --------------PARAMETRI E STRUTTURE DATI GLOBALI--------------------------------------------------------------------------------
int sequenceNumber = 0;                                 // serve al sequenziatore per impartire l'ordine totale
int myNodeId = 0;                                       // id del nodo corrente
int messageId = 0;                                      // id del messaggio
String myMacAddress = "";                               // indirizzo mac del nodo corrente, verrà inizializzato in setup()
int vectorClock[NUM_NODES] = {0,0,0,0,0};               // serve a tutti i partecipanti per avere un ordine causale
int highestBid = 0;                                     // segna il valore dell'offerta più alta attuale
int myHighestBid = 0;                                   // segna il valore dell'offerta fatta dal nodo corrente più alta , per LCD
int winnerNodeId = -1;                                  // id del vincitore dell'asta attuale
unsigned long auctionEndTime = 0;                       // tempo di fine asta
unsigned long restartTimer = 0;                         // con la funzione millis() aggiorno ogni volta quando mi arriva un offerta, mi serve per implementare un timer
unsigned long lastDebounceTimeBid = 0;                  // lastDebounceTime per il bottone di offerta
unsigned long lastDebounceTimeStart = 0;                // lastDebounceTime per il bottone di inizio asta
int lastDebounceStateBid = LOW;                         // lastDebounceState per il bottone di offerta
int lastDebounceStateStart = LOW;                       // lastDebounceState per il bottone di inizio asta
int buttonStateBid = LOW;                               // buttonState per il bottone di offerta
int buttonStateStart = LOW;                             // buttonState per il bottone di inizio asta
unsigned long debounceDelay = 100;                      // debounceDelay per il bottone di offerta
bool auctionStarted = false;                            // Flag per tracciare se l'asta è partita
bool isSending = false;                                 // Lock LOGICO per evitare che due messaggi vengano inviati contemporaneamente
bool lastWasBid = false;

// Struttura per messaggi scambiati dai nodi
typedef struct struct_message {
    int bid = 0;                                            // bid dell'offerta nel messaggio
    int highestBid = 0;                                     // offerta più alta attuale
    int messageId = 0;                                      // id del messaggio, utile per riconoscere i messaggi in fase di ricezione
    int senderId = 0;                                       // id del mittente del messaggio
    int sequenceNum = 0;                                    // sequence number associato al messaggio
    int vectorClock[NUM_NODES] = {0,0,0,0,0};               // vector clock inviato nel messaggio
    String messageType = "";                                // tipo di messaggio ("bid", "order")
} struct_message;

//Coda dei messaggi in attesa
std::vector<struct_message> holdBackQueueSeq;           // Hold-back queue Sequenziatore
std::vector<struct_message> holdBackQueuePart;          // Hold-back queue Partecipanti
std::vector<struct_message> holdBackQueueOrder;         // Hold-back queue messaggi di ordinamento da parte del sequenziatore, la sfruttano solo i partecipanti semplici
std::vector<struct_message> holdBackQueueCausal;        // Hold-back queue messaggi causali che non hanno ancora ricevuto l'ordinamento

struct_message auctionMessageToSend;                    //Messaggio da inviare (bid)
struct_message auctionMessageToSendOrder;               //Messaggio da inviare (order)
struct_message auctionMessageToReceive;                 //Messaggio ricevuto

std::queue<struct_message> message_queue_to_send;       // Coda dei messaggi da inviare per il sequenziatore

//-----------------------------------------------------------------------------------------------------------------

//----------------------- CONFIGURAZIONE AMBIENTE TASK ------------------------------------------------------------

// Tipi che possono essere associati ad un task
enum CallbackType {
    SEND_BID,                                            // Tipo di task per inviare un'offerta
    ON_DATA_RECEIVE                                      // Tipo di task per ricevere un'offerta       
};

// Struttura per task passato nella coda dei task
typedef struct {
    CallbackType type;                                    // Tipo di task 
    struct_message message;                               // Messaggio associato al task
} CallbackMessage;

QueueHandle_t callbackQueue;                              // Coda per le richieste delle callback
TaskHandle_t callbackTaskHandle;                          // Handle del task

esp_now_peer_info_t peerInfo;                             // Aggiunta dichiarazione della variabile peerInfo

//--------------------------------------------------------------------------------------------------------------

// ----------------------- DICHIARAZIONE FUNZIONI ASSOCIATE AI TASK-----------------------------------------------------------

// CallBack principale per l'esecuzione dei task
void callbackTask(void *pvParameters) {
    CallbackMessage msg;

    while (true) {
        // Attende un messaggio nella coda
        if (xQueueReceive(callbackQueue, &msg, portMAX_DELAY) == pdTRUE) {
            // Gestisce il messaggio in base al tipo
            switch (msg.type) {
                case SEND_BID:
                    sendBid(msg.message);
                    break;
                case ON_DATA_RECEIVE:
                    onDataReceive(msg.message); // Esegui con i dati
                    break;
            }

        }
    }
}

// Funzione per inviare un'offerta
void sendBid(struct_message message){

  // Se sono il sequenziatore pusho nella coda di invio del sequenziatore
  if(myNodeId == 0){
    queueMessage(message);                                
  }else{

    // Altrimenti invio il messaggio
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &message, sizeof(auctionMessageToSend));

    if (result == ESP_OK) {
      Serial.println("[Partecipant] Offerta inviata di " + String(message.bid) + " da parte di " + String(message.senderId));
    } else {
      Serial.println("[Partecipant] Errore nell'invio del messaggio di offerta");
    }
  }

  // Sto inviando un'offerta e a prescindere da chi sono, devo mantenere l'ordinamento causale
  // Simulo la ricezione del messaggio che sto inviando a me stesso
  onDataReceive(message);

}

// Funzione per la ricezione di un messaggio 
void onDataReceive(struct_message message) {

    Serial.println("");
    Serial.println("OnDataReceive!");

    // Se sono il sequenziatore faccio una receive diversa
    if(myMacAddress == mac_sequencer){

      // Aggiungo il messaggio alla coda di messaggi del sequenziatore
      holdBackQueueSeq.push_back(message);                              

      // Stampe per il log
      Serial.println("[Sequencer] Messaggio aggiunto alla holdBackQueue con:");
      Serial.println("SenderId: "+String(message.senderId));
      Serial.println("MessageId: "+String(message.messageId));
      Serial.println("Bid: "+String(message.bid));
      // Stampa il vector clock
      Serial.print("Vector Clock: [ ");
      for (int i = 0; i < NUM_NODES; i++) { 
        Serial.print(message.vectorClock[i]);
        if (i < NUM_NODES - 1) Serial.print(", "); 
      }
      Serial.println(" ]");
      Serial.println("[Sequencer] La HoldBackQueue è la seguente");
      printHoldBackQueueSeq();

      // Processo la coda per controllare la causalità del messaggio
      processHoldBackQueue(holdBackQueueSeq, true); // coda del sequenziatore, true per il sequenziatore

    // Se sono un partecipante semplice faccio un'altra receive
    }else{

      // Se il messaggio è di tipo "bid" lo aggiungo alla hold-back queue per controllare causalità
      if(message.messageType == "bid"){                                

        // Lo aggiungo alla coda dei messaggi dei partecipanti
        holdBackQueuePart.push_back(message);                 

        // Stampe per il log
        Serial.println("[Partecipant] Messaggio aggiunto alla hold-back queue con:");
        Serial.println("SenderId: "+String(message.senderId));
        Serial.println("MessageId: "+String(message.messageId));
        Serial.println("Bid: "+String(message.bid));
        // Stampa il vector clock
        Serial.print("Vector Clock: [ ");
        for (int i = 0; i < NUM_NODES; i++) { // Usa NUM_NODES per la dimensione dinamica
          Serial.print(message.vectorClock[i]);
          if (i < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
        }
        Serial.println(" ]");
        Serial.println("[Partecipant] La HoldBackQueue è la seguente");
        printHoldBackQueuePart();

        // Quando mi arriva un messaggio controllo tutta la coda
        // Così a partire dall'ultimo vedo se è causale, e se il suo invio mi ha "sbloccato" altri in coda
        // Inoltre se durante il controllo faccio un erase di un messaggio causale, mi conviene ricominciare il ciclo
        // infatti è possibile che quelli dietro adesso siano causali e quindi sbloccabili

        // Processo la coda dei partecipanti per controllare causalità
        processHoldBackQueue(holdBackQueuePart, false); // coda dei partecipanti, false per i partecipanti

      // Se il messaggio è di ordinamento lo aggiungo alla hold-back queue per controllare ordinamento
      }else if(message.messageType == "order"){

        // Stamper per il log
        Serial.println("[Partecipant] Arrivato un messaggio di ordinamento");
        Serial.println("SenderId: "+String(message.senderId));
        Serial.println("MessageId: "+String(message.messageId));
        Serial.println("Sequence Number: "+String(message.sequenceNum));
        Serial.println("[Partecipant] La HoldBackQueue di ordinamento è la seguente");
        printHoldBackQueueOrder();

        // Controllo se è già presente il corrispettivo messaggio nella coda dei messaggi causali
        bool firstCorrispondence = checkCorrispondence(message,"fromOrderToCausal");

        // Il risultato sarà true se il messaggio di ordinamento ha trovato il suo corrispettivo
        // Quindi deve fare la TO Deliver, dopo la deliver potrei aver "sbloccato" altri messaggi
        if(firstCorrispondence){
          TO_Deliver(message);
          Serial.println("[Partecipant] Ho fatto la TO Deliver");
          Serial.println("[Partecipant] La HoldBackQueue di ordinamento è diventata");
          printHoldBackQueueOrder();
        }else{
          holdBackQueueOrder.push_back(message);
          Serial.println("[Partecipant] Messaggio aggiunto alla hold-back queue di ordinamento.");
          Serial.println("[Partecipant] La HoldBackQueue di ordinamento è diventata");
          printHoldBackQueueOrder();
        }

        // Controllo se il messaggio di ordinamento mi ha sbloccato qualcosa
        // Qualche ordinamento che ha il sequence number più alto, ma che è arrivato prima ad esempio
        if(firstCorrispondence){
          bool checkPopCorrispondence = false;

          // Finchè trovo messaggi che possono essere sbloccati, li sblocco
          do{
            checkPopCorrispondence = false;

            // Scorro la coda dei messaggi di ordinamento
            for(auto it = holdBackQueueOrder.rbegin(); it != holdBackQueueOrder.rend(); ){

              // Controllo se il messaggio può essere sbloccato
              if(checkCorrispondence(*it,"fromCausalToOrder")){

                // Se posso sbloccare il messaggio, lo elimino dalla coda facendo la TO Deliver
                checkPopCorrispondence = true;
                TO_Deliver(*it);
                Serial.println("[Partecipant] Ho fatto la TO Deliver");
                Serial.println("[Partecipant] La HoldBackQueue di ordinamento è diventata");
                printHoldBackQueueOrder();

                // Ricomincio il ciclo perchè potrei aver sbloccato altri messaggi che ho già controllato nel ciclo for
                break;
              }
              ++it;
            }
          }while(checkPopCorrispondence);
        }

      // Se il messaggio è di inizio asta faccio il set di tutte le varibili e strutture dati 
      }else if(message.messageType == "start"){
        startAuction();
        auctionStarted = true; // Setto l'asta come iniziata, abilito la lettura dei bottoni per i partecipanti
        Serial.println("[Partecipant] Asta iniziata sono così felice");

      // Se il messaggio è di fine asta stampo info 
      }else if(message.messageType == "end"){
        auctionStarted = false; // Disabilito la lettura dei bottoni per i partecipanti
        Serial.println("[Partecipant] Asta finita, sono triste");
        printHoldBackQueues();
      }
    }
}


//----------------------------------------------------------------------------------------------------------------------------

//------------------------------FUNZIONE PER INVIO SEQUENZIATORE--------------------------------------------------------------

void queueMessage(struct_message message){
  message_queue_to_send.push(message);
  processQueue();
}

void processQueue(){
  if(!isSending && !message_queue_to_send.empty()){
    isSending = true;
    struct_message message = message_queue_to_send.front();
    message_queue_to_send.pop();
    lastWasBid = (message.messageType == "bid");
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &message, sizeof(message));
    if (result == ESP_OK) {
      Serial.println("[Sequencer] Messaggio inviato con successo con bid: "+String(message.bid)+" da "+String(message.senderId));
      Serial.println("[Sequencer] di tipo "+message.messageType);
    } else {
      Serial.println("[Sequencer] Errore nell'invio del messaggio da parte di "+String(message.senderId));
    }

    // Faccio l'invio del messaggio di ordinamento al serverDashboard
    if(message.messageType != "bid"){
      sendAuctionStateToServer(message);
    }
    
    isSending = false;
  }
}

void onSendComplete() {
  isSending = false;
  processQueue();
}

//----------------------------------------------------------------------------------------------------------------------------------

//-----------------------------FUNZIONI TRIGGER PER I TASK---------------------------------------------------------------------------

// Queste funzioni servono per rendere a stessa priorità l'invio di un messaggio e la ricezione di un messaggio
// Abbiamo dovuto implementare questo meccanismo a causa delle limitazioni di ESP-NOW e perchè l'invio di un messaggio
// non era protetto da nulla e poteva essere prelazionato dalla ricezione e conseguente elaborazione di un'offerta

// Callback di ESP-NOW, viene chiamata quando invio un messaggio con ESP-NOW
void triggerOnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status){
  // Quando invio un messaggio non faccio niente
  // Il task ha già ricevuto per me il messaggio che invio
}

// Callback di ESP-NOW, viene chiamata quando ricevo un messaggio con ESP-NOW
void triggerOnDataReceive(const uint8_t *mac, const uint8_t *incomingData, int len){
  memcpy(&auctionMessageToReceive, incomingData, sizeof(auctionMessageToReceive));

  // Creo un messaggio di callback per il task
  CallbackMessage msg;
  msg.type = ON_DATA_RECEIVE;
  msg.message = auctionMessageToReceive;

  // Inserisco il messaggio nella coda dei task
  xQueueSend(callbackQueue, &msg, portMAX_DELAY);

}

// Funzione per generare e inviare un'offerta
void triggerSendBid(){

  // Costruisco il messaggio di oggerta
  struct_message message;
  message.bid = highestBid+1;                                             // L'offerta è la più alta + 1
  myHighestBid = highestBid;                                              // Salvo la mia offerta più alta    
  message.senderId = myNodeId;                                            // Il mittente è il mio id  
  message.messageId = messageId+1;                                        // Incremento l'id del messaggio
  messageId++;
  for (int i = 0; i < NUM_NODES; i++) {                                   // Copio il vector clock           
    message.vectorClock[i] = vectorClock[i];
  }
  message.vectorClock[myNodeId] = vectorClock[myNodeId] + 1;              // Incremento il mio valore del vector clock 
  message.messageType = "bid";                                            // Setto il tipo di messaggio     

  // Creo un messaggio di callback per il task
  CallbackMessage msg;
  msg.message = message;
  msg.type = SEND_BID;

  // Inserisco il messaggio nella coda dei task
  xQueueSend(callbackQueue, &msg, portMAX_DELAY);

}

//----------------------------------------------------------------------------------------------------------------------------------

// Callback invio dati - TUTTI
void OnDataSent(struct_message message) {
  // Quando invio un'offerta me la registro nella coda
  // Come se inviassi il messaggio a me stesso

  Serial.println("");
  Serial.println("OnDataSent!");
  // Se sono il sequenziatore e l'ultimo messaggio inviato era un'offerta, la ricevo
  if(myMacAddress == mac_sequencer && lastWasBid){
    Serial.println("OnDataSent: BID-SEQUENCER!");
    holdBackQueueSeq.push_back(message);                                 // Se sono il sequenziatore pusho nella mia coda
    Serial.println("[Sequencer] Messaggio aggiunto alla holdBackQueue con:");
    Serial.println("SenderId: "+String(message.senderId));
    Serial.println("MessageId: "+String(message.messageId));
    Serial.println("Bid: "+String(message.bid));
    // Stampa il vector clock
    Serial.print("Vector Clock: [ ");
    for (int i = 0; i < NUM_NODES; i++) {                                             // Usa NUM_NODES per la dimensione dinamica
      Serial.print(message.vectorClock[i]);
      if (i < NUM_NODES - 1) Serial.print(", ");                                      // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
    Serial.println("[Sequencer] La HoldBackQueue è la seguente");
    printHoldBackQueueSeq();

    processHoldBackQueue(holdBackQueueSeq, true);                                     // Controllo la coda di messaggi


  }else if(myMacAddress != mac_sequencer){
    Serial.println("OnDataSent: BID-PARTECIPANT!");
    holdBackQueuePart.push_back(message);                                // Se sono un partecipante generico, pusho nella coda partecipanti
    Serial.println("[Partecipant] Messaggio aggiunto alla hold-back queue con:");
    Serial.println("SenderId: "+String(message.senderId));
    Serial.println("MessageId: "+String(message.messageId));
    Serial.println("Bid: "+String(message.bid));
    // Stampa il vector clock
    Serial.print("Vector Clock: [ ");
    for (int i = 0; i < NUM_NODES; i++) { // Usa NUM_NODES per la dimensione dinamica
      Serial.print(message.vectorClock[i]);
      if (i < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
    Serial.println("[Partecipant] La HoldBackQueue è la seguente");
    printHoldBackQueuePart();

    processHoldBackQueue(holdBackQueuePart, false); // Controllo la coda di messaggi
    Serial.println("\r\nStatus BID-PARTECIPANT:\t");
  }

  if(myNodeId==0){
    onSendComplete();
  }

}

//-----------------------------FUNZIONI DI GESTIONE CODE DI MESSAGGI-------------------------------------------------------------------

// Funzione che processa una coda di messaggi controllando se c'è un messaggio che è causale 
bool processHoldBackQueue(std::vector<struct_message> &holdBackQueue, bool isSequencer){
  bool checkPop = false;

  do{ // Ciclo finchè non ho eliminato tutti i messaggi sbloccati
    Serial.println((isSequencer ? "[Sequencer] " : "[Partecipant] ") + String("Nella mia coda ho ") + String(holdBackQueue.size()) + String(" messaggi"));
    checkPop = false;
    for (auto it = holdBackQueue.rbegin(); it != holdBackQueue.rend(); ){
      // Faccio il controllo per il messaggio in coda, ultimo posto (se è arrivato un messaggio è quello in ultima posizione)
      if(isSequencer){
        checkPop = causalControl(*it,it); // Controllo specifico per il sequenziatore
      }else{
        checkPop = causalControlPartecipant(*it,it); // Controllo specifico per i partecipanti
      }

      Serial.println((isSequencer ? "[Sequencer] " : "[Partecipant] ") + String("checkPop: ") + String(checkPop));
      if(checkPop){ // se ho sbloccato un messaggio...
        Serial.println((isSequencer ? "[Sequencer] " : "[Partecipant] ") + String("Il pop era true, ricomnicio il ciclo"));
        break;      // esco dal ciclo per ricominciare dalla fine della coda
      }
      ++it;           // Incremento l'iteratore per scorrere la coda
    }
  }while(checkPop);

  return checkPop;
}

// Funzione che controlla se un messaggio di ordinamento ha il suo corrispettivo nella coda dei messaggi causali
// Oppure controlla se un messaggio causale ha il suo corrispettivo nella coda dei messaggi di ordinamento
bool checkCorrispondence(struct_message messageToCheck, String corrispondenceType){

  if (corrispondenceType == "fromCausalToOrder"){
    Serial.println("[Partecipant] Controllo corrispondenza tra i messaggi nella coda dei messaggi di Ordinamento");
    Serial.println("[Partecipant] La coda di attesa di ordinamento ha "+String(holdBackQueueOrder.size())+" messaggi");

    try{
    for (int i=0; i<holdBackQueueOrder.size(); i++) {
      if (messageToCheck.messageId == holdBackQueueOrder[i].messageId && messageToCheck.senderId == holdBackQueueOrder[i].senderId && sequenceNumber == holdBackQueueOrder[i].sequenceNum ) {
        return true;
      }
    }
    return false;
    }catch(const std::out_of_range& e){
      Serial.println("[Partecipant] Errore nella funzione checkCorrispondence");
    }
  }else if(corrispondenceType == "fromOrderToCausal"){
    Serial.println("[Partecipant] Controllo corrispondenza tra i messaggi nella coda dei messaggi Causali");
    Serial.println("[Partecipant] La coda di attesa dei causali ha "+String(holdBackQueueCausal.size())+" messaggi");

    try{
    for (int i=0; i<holdBackQueueCausal.size(); i++) {
      if (messageToCheck.messageId == holdBackQueueCausal[i].messageId && messageToCheck.senderId == holdBackQueueCausal[i].senderId && sequenceNumber == messageToCheck.sequenceNum ) {
        return true;
      }
    }
    return false;
    }catch(const std::out_of_range& e){
      Serial.println("[Partecipant] Errore nella funzione checkCorrispondence");
    }
  }
}

// [SEQUENZIATORE] Funzione per controllare se un singolo messaggio è causale, passo il messaggio e puntatore ad esso
bool causalControl(struct_message messageToCheck, std::vector<struct_message>::reverse_iterator it){
  if((messageToCheck.vectorClock[messageToCheck.senderId] == vectorClock[messageToCheck.senderId] + 1) && isCausallyRead(messageToCheck)){
    Serial.println("[Sequencer] Messaggio causale da parte di "+String(messageToCheck.senderId)+" con offerta "+String(messageToCheck.bid)+" sbloccato");
    holdBackQueueSeq.erase(it.base());                                                                  
    Serial.println("[Sequencer] Ho eliminato questo messaggio causale");
    CO_Deliver(messageToCheck);                                                                   
    return true;
  }
  return false;
}

// [PARTECIPANTE SEMPLICE] Funzione per controllare se un singolo messaggio è causale, passo il messaggio e puntatore ad esso
bool causalControlPartecipant(struct_message messageToCheck, std::vector<struct_message>::reverse_iterator it){ 
  if((messageToCheck.vectorClock[messageToCheck.senderId] == vectorClock[messageToCheck.senderId] + 1) && isCausallyRead(messageToCheck)){
  Serial.println("[Partecipant] Messaggio causale da parte di "+String(messageToCheck.senderId)+" con offerta "+String(messageToCheck.bid)+" sbloccato");
    holdBackQueuePart.erase(it.base());                                                                 
    Serial.println("[Partecipant] Ho eliminato questo messaggio causale");
    holdBackQueueCausal.push_back(messageToCheck);                                                
    Serial.println("[Partecipant] Ho aggiunto il messaggio alla seconda coda di attesa, aspetto mess di ordinamento");
    CO_DeliverPartecipant(messageToCheck);
    Serial.println("[Partecipant] Ho fatto CO Deliver, aggiornato il vector clock");
    // FAI PARTIRE LA TO DELIVER
    if(checkCorrispondence(messageToCheck,"fromCausalToOrder")){
      Serial.println("[Partecipant] ho ordinamento e causale, posso fare la TO Deliver");
      TO_Deliver(messageToCheck);
    }
    return true;
  }
  return false;
}

// Funzione che controlla la seconda condizione di causalità
// Il valore del vector clock di tutte le altre posizioni deve essere minore o uguale al mio valore locale delle altre posizioni
bool isCausallyRead(struct_message messageToCheck){
  for (int i = 0; i < NUM_NODES; i++) {
    if(i == messageToCheck.senderId){ //Non controllo il nodo che ha inviato il messaggio
      continue;

    }else if (messageToCheck.vectorClock[i] > vectorClock[i]) {
      return false;
    }
  }
  return true;
}

//----------------------------------------------------------------------------------------------------------------------------------

// -----------------------------FUNZIONI DI DELIVER--------------------------------------------------------------------------------

// [PARTECIPANTE SEMPLICE] Funzione per la CO Deliver dei messaggi
void CO_DeliverPartecipant(struct_message message){
  vectorClock[message.senderId]++;
}

// [SEQUENZIATORE] Funzione per la CO Deliver dei messaggi
void CO_Deliver(struct_message message){
  vectorClock[message.senderId]++;                // Aggiorno il vector clock alla mia posizione
  sendSequencer(message);                         // Invio il messaggio di ordinamento
}

// Funzione per la TO Deliver dei partecipanti
void TO_Deliver(struct_message message){
  int idToDelete = message.messageId;
  int senderIdToDelete = message.senderId;

  //elimino in entrambe le code, sia di ordinamento che in quella causale
  for(int i=0; i<holdBackQueueOrder.size(); i++){
    if(holdBackQueueOrder[i].messageId == idToDelete && holdBackQueueOrder[i].senderId == senderIdToDelete){
      holdBackQueueOrder.erase(holdBackQueueOrder.begin()+i);
    }
  }

  for(int i=0; i<holdBackQueueCausal.size(); i++){
    if(holdBackQueueCausal[i].messageId == idToDelete && holdBackQueueCausal[i].senderId == senderIdToDelete){
      holdBackQueueCausal.erase(holdBackQueueCausal.begin()+i);
    }
  }

  //aggiorno il sequence number
  sequenceNumber++;

  // Controllo se la Highest Bid è cambiata
  if(message.bid > highestBid){                                      // Se la bid che ho prelevato è più grande...
    highestBid = message.bid;                                        // Aggiorno l'offerta più alta la momento
    winnerNodeId = message.senderId;                                 // e il vincitore momentaneo
  }


  Serial.println("[Partecipant] Messaggio consegnato");
  Serial.println("[Partecipant] Bid offerta " + String(message.bid) + "da parte di " + String(message.senderId));
  Serial.println("[Partecipant] Il mio Sequence Number ora è " + String(sequenceNumber));

}

//------------------------------------------------------------------------------------------------------------------------------------

//-----------------------------FUNZIONI ESCLUSIVE SEQUENZIATORE-----------------------------------------------------------------------

// Funzione per inviare un messaggio di ordinamento
void sendSequencer(struct_message message) {

    struct_message orderMessage = message;
    orderMessage.messageType = "order";                                             // Setto il tipo di messaggio

    Serial.println("sendSequencer - Received bid: " + String(orderMessage.bid));
    Serial.println("sendSequencer - Current highestBid: " + String(highestBid));
    Serial.println("sendSequencer - Message senderId: " + String(orderMessage.senderId));
    Serial.println("sendSequencer - Current winnerNodeId: " + String(winnerNodeId));

    // Controllo se la Highest Bid è cambiata
    if(orderMessage.bid > highestBid){                                      // Se la bid che ho prelevato è più grande...
      highestBid = orderMessage.bid;                                        // Aggiorno l'offerta più alta la momento
      winnerNodeId = orderMessage.senderId;                                 // e il vincitore momentaneo
      orderMessage.highestBid = highestBid;                                 // Aggiorno il messaggio con l'offerta più alta
    }

    Serial.println("sendSequencer - After update winnerNodeId: " + String(winnerNodeId));

    orderMessage.sequenceNum = sequenceNumber;
    //esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &auctionMessageToSendOrder, sizeof(auctionMessageToSendOrder));
    queueMessage(orderMessage);
    sequenceNumber++;

    restartTimer = millis();                                                // Aggiorno il timer di restart
    
}

// Funzione per inviare lo stato dell'asta al server
void sendAuctionStateToServer(struct_message msg) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return;
    }

    HTTPClient http;
    Serial.print("Connecting to: ");
    Serial.println(serverUrl);
    
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["bid"] = msg.bid;
    doc["highest_bid"] = highestBid;
    doc["message_id"] = msg.messageId;
    doc["sender_id"] = msg.senderId;
    doc["sequence_number"] = msg.sequenceNum;  
    doc["winner_id"] = winnerNodeId;          
    doc["message_type"] = msg.messageType;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    Serial.print("Sending JSON: ");
    Serial.println(jsonString);

    int httpResponseCode = http.POST(jsonString);
    
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("HTTP Response code: %d\n", httpResponseCode);
        Serial.println("Server Response: " + response);
    } else {
        Serial.printf("Error sending to server. Error: %d\n", httpResponseCode);
        Serial.println(http.errorToString(httpResponseCode));
    }

    http.end();
}

//------------------------------------------------------------------------------------------------------------------------------------

//-----------------------------FUNZIONI DI GESTIONE ASTA------------------------------------------------------------------------------

// Funzione per iniziare l'asta - TUTTI
void startAuction(){
  highestBid = 0;
  winnerNodeId = -1;
  sequenceNumber = 0;
  messageId = 0;
  auctionStarted = true;                                                            // metto a true l'inizio dell'asta
  restartTimer = millis();                                                          // leggo e salvo il tempo di inizio asta
  auctionEndTime = DURATION_TIME;
  for(int i=0; i<NUM_NODES; i++){
    vectorClock[i] = 0;                                                             //resetto il vector clock
  }
  holdBackQueueSeq.clear();                                                         // pulisco la coda di messaggi
  holdBackQueuePart.clear();                                                        // pulisco la coda di messaggi
  holdBackQueueOrder.clear();                                                       // pulisco la coda di messaggi
  holdBackQueueCausal.clear();                                                      // pulisco la coda di messaggi

  auctionMessageToSend.messageId = 0;                                               // resetto l'id del messaggio
  auctionMessageToSend.bid = 0;                                                     // resetto l'offerta
  for(int i=0; i<NUM_NODES; i++){
    auctionMessageToSend.vectorClock[i] = 0;
  }

  auctionMessageToSendOrder.messageId = 0;
  auctionMessageToSendOrder.bid = 0;
  auctionMessageToSendOrder.messageType = "order";
  for(int i=0; i<NUM_NODES; i++){
    auctionMessageToSendOrder.vectorClock[i] = 0;
  }

  auctionMessageToReceive.messageId = 0;                                               // resetto l'id del messaggio
  auctionMessageToReceive.bid = 0;                                                      // resetto l'offerta
  for(int i=0; i<NUM_NODES; i++){
    auctionMessageToReceive.vectorClock[i] = 0;
  }


  Serial.println("[Sequencer] Asta iniziata");
}


// Funzione che monitora la fine dell'asta
void checkEndAuction(){

  if(millis() - restartTimer >= DURATION_TIME){                                    // Se il tempo attuale (millis()) meno il tempo di inizio asta (restartTimer) è maggiore di Duration
    auctionStarted = false;                                                        // L'asta è finita, tutti a casa, LUKAKU è mio!!
    Serial.println("Ha vinto il nodo " + String(winnerNodeId));                      // Annuncio il vincitore
    Serial.println("con un offerta di " + String(highestBid));
    auctionMessageToSend.messageType = "end";                                       // Setto il messaggio di fine asta
    //esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &auctionMessageToSend, sizeof(auctionMessageToSend)); // Invio il messaggio di fine asta
    queueMessage(auctionMessageToSend);
  }

}

// Funzione di debounce con switch-case
bool checkButtonPressed(int pinButton) {
  // Leggo lo stato attuale del pulsante
  int reading = digitalRead(pinButton);

  // Switch per distinguere i pulsanti
  switch (pinButton) {
    case BUTTON_AUCTION_PIN:
      // Se il pulsante Auction ha cambiato stato
      if (reading != lastDebounceStateStart) {
        lastDebounceTimeStart = millis(); // Aggiorno il timer di debounce
      }

      // Controllo se il debounce è superato
      if ((millis() - lastDebounceTimeStart) > debounceDelay) {
        if (reading != buttonStateStart) {
          buttonStateStart = reading;

          if (buttonStateStart == LOW) {
            Serial.println("[Sequencer] Pulsante Auction premuto!");
            lastDebounceStateStart = reading;
            return true;
          }
        }
      }

      lastDebounceStateStart = reading;
      break;

    case BUTTON_BID_PIN:
      // Se il pulsante Bid ha cambiato stato
      if (reading != lastDebounceStateBid) {
        lastDebounceTimeBid = millis(); // Aggiorno il timer di debounce
      }

      // Controllo se il debounce è superato
      if ((millis() - lastDebounceTimeBid) > debounceDelay) {
        if (reading != buttonStateBid) {
          buttonStateBid = reading;

          if (buttonStateBid == LOW) {
            Serial.println("Pulsante Bid premuto!");
            lastDebounceStateBid = reading;
            return true;
          }
        }
      }

      lastDebounceStateBid = reading;
      break;

    default:
      // Caso di default, nessuna azione
      break;
  }

  return false; // Nessun pulsante premuto
}

// Funzione di debug per stampare le code di messaggi dei partecipanti
void printHoldBackQueues(){

  printHoldBackQueuePart();
  printHoldBackQueueCausal();
  printHoldBackQueueOrder();

}

void printHoldBackQueuePart(){

  Serial.println("HoldBackQueuePart:");
  for (int i = 0; i < holdBackQueuePart.size(); i++) {
    Serial.println("MessageId: "+String(holdBackQueuePart[i].messageId));
    Serial.println("SenderId: "+String(holdBackQueuePart[i].senderId));
    Serial.println("Bid: "+String(holdBackQueuePart[i].bid));
    Serial.print("Vector Clock: [ ");
    for (int j = 0; j < NUM_NODES; j++) { // Usa NUM_NODES per la dimensione dinamica
      Serial.print(holdBackQueuePart[i].vectorClock[j]);
      if (j < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
  }

}

void printHoldBackQueueCausal(){

  Serial.println("HoldBackQueueCausal:");
  for (int i = 0; i < holdBackQueueCausal.size(); i++) {
    Serial.println("MessageId: "+String(holdBackQueueCausal[i].messageId));
    Serial.println("SenderId: "+String(holdBackQueueCausal[i].senderId));
    Serial.println("Bid: "+String(holdBackQueueCausal[i].bid));
    Serial.print("Vector Clock: [ ");
    for (int j = 0; j < NUM_NODES; j++) { // Usa NUM_NODES per la dimensione dinamica
      Serial.print(holdBackQueueCausal[i].vectorClock[j]);
      if (j < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
  }

}

void printHoldBackQueueOrder(){

  Serial.println("HoldBackQueueOrder:");
  for (int i = 0; i < holdBackQueueOrder.size(); i++) {
    Serial.println("MessageId: "+String(holdBackQueueOrder[i].messageId));
    Serial.println("SenderId: "+String(holdBackQueueOrder[i].senderId));
    Serial.println("Bid: "+String(holdBackQueueOrder[i].bid));
    Serial.print("Vector Clock: [ ");
    for (int j = 0; j < NUM_NODES; j++) { // Usa NUM_NODES per la dimensione dinamica
      Serial.print(holdBackQueueOrder[i].vectorClock[j]);
      if (j < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
  }

}

void printHoldBackQueueSeq(){

  Serial.println("HoldBackQueueSeq:");
  for (int i = 0; i < holdBackQueueSeq.size(); i++) {
    Serial.println("MessageId: "+String(holdBackQueueSeq[i].messageId));
    Serial.println("SenderId: "+String(holdBackQueueSeq[i].senderId));
    Serial.println("Bid: "+String(holdBackQueueSeq[i].bid));
    Serial.print("Vector Clock: [ ");
    for (int j = 0; j < NUM_NODES; j++) { // Usa NUM_NODES per la dimensione dinamica
      Serial.print(holdBackQueueSeq[i].vectorClock[j]);
      if (j < NUM_NODES - 1) Serial.print(", "); // Aggiungi virgola tra i valori, ma non alla fine
    }
    Serial.println(" ]");
  }

}

//------------------------------------------------------------------------------------------------------------------------------------

//--------------------------------------FUNZIONE DI SETUP---------------------------------------------------------------------------
void setup() {

  // Aggiungi alcune associazioni MAC address -> numero
  macToNumberMap["F8:B3:B7:2C:71:80"] = 0; 
  macToNumberMap["F8:B3:B7:44:BF:C8"] = 1;
  macToNumberMap["4C:11:AE:65:AF:08"] = 2; 
  macToNumberMap["4C:11:AE:B3:5A:8C"] = 3;
  macToNumberMap["A0:B7:65:26:88:D4"] = 4;

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(2000);
  myMacAddress = WiFi.macAddress();
  Serial.println("MAC Address: " + myMacAddress);
  Serial.println("My Node ID: " + String(macToNumberMap[myMacAddress]));

  // DA CAMBIARE SE SI CAMBIA HOTSPOT E NON HA STESSO CANALE WI FI
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); 

  // Assegno l'id del nodo in base al MAC address
  myNodeId = macToNumberMap[myMacAddress]; 

  // Inizializza la coda dei task
  callbackQueue = xQueueCreate(10, sizeof(CallbackMessage));
  if (callbackQueue == NULL) {
      Serial.println("Errore nella creazione della coda");
      return;
  }

  // Crea il task per gestire le callback
  xTaskCreatePinnedToCore(
        callbackTask,
        "CallbackTask",
        4096,
        NULL,
        configMAX_PRIORITIES - 1,
        &callbackTaskHandle,
        1
  );

  // Configurazione del bottone per l'inizio dell'asta e della connessione wi-fi
  if (myNodeId == 0) {
    pinMode(BUTTON_AUCTION_PIN, INPUT_PULLUP);

    // Il sequenziatore deve fare anche da station, riconfigurazione necessaria
    WiFi.mode(WIFI_AP_STA);
    delay(2000);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi!");
    Serial.println("Connesso al Wi-Fi. Canale: " + String(WiFi.channel()));
    
  }else{
    Serial.println("Connesso al Wi-Fi. Canale: " + String(WiFi.channel()));
  }


  //Configurazione del bottone per l'offerta
  pinMode(BUTTON_BID_PIN, INPUT_PULLUP);

  if (esp_now_init() != ESP_OK) {                                                   // Se la connesione esp non è andata a buon fine
        Serial.println("Error initializing ESP-NOW");                               // lo segnalo e termino il programma
        return;
  }

  esp_now_register_send_cb(triggerOnDataSent);                                      // registro la funzione di callback per invio messaggi

  memcpy(peerInfo.peer_addr, broadcastAddress, 6);                                  // copio le informazione dei peer nelle locazioni dei peer address
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add peer");
        return;
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(triggerOnDataReceive));                // registro la funzione di callback per ricezione messaggi

  // Inizializza la comunicazione I2C e il display LCD
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight(); // Accende la retroilluminazione

  // Mostra un messaggio sul display
  lcd.setCursor(0, 0);                                                              // Posiziona il cursore sulla prima colonna della prima riga
  lcd.print("My Node ID: "+String(myNodeId));
  lcd.setCursor(0, 1);                                                              // Posiziona il cursore sulla prima colonna della seconda riga
  lcd.print("HB: " + String(highestBid) + " - LB: " + String(myHighestBid));

  delay(1000);

}

//------------------------------------------------------------------------------------------------------------------------------------

//--------------------------------FUNZIONE DI LOOP -------------------------------------------------------------------------------
void loop() {

  // Gestione dei led
  lcd.setCursor(0, 0); 
  lcd.print("My Node ID: "+String(myNodeId));
  lcd.setCursor(0, 1); 
  lcd.print("HB: " + String(highestBid) + " - LB: " + String(myHighestBid));  

  //### COMPORTAMENTO DEL SEQUENZIATORE ###
  if(myNodeId == 0){

    // Controllo se il bottone di inizio asta è stato premuto e se non è già in corso un'asta
    if(checkButtonPressed(BUTTON_AUCTION_PIN) && !auctionStarted){
      startAuction();                                                           // setto le variabili iniziali, tra cui la variabile che segna l'inizio dell'asta
      auctionMessageToSend.messageType = "start";                               // setto il messaggio di inizio asta
      //esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &auctionMessageToSend, sizeof(auctionMessageToSend)); // invio il messaggio di inizio asta
      queueMessage(auctionMessageToSend);
    }

    // Se l'asta è iniziata
    if(auctionStarted){                                                               

      // Controllo se l'asta è finita
      checkEndAuction();                                                              

      // Se bottone premuto per fare offerta
      if(checkButtonPressed(BUTTON_BID_PIN)){
        triggerSendBid();   
      }

    }

  //#### COMPORTAMENTO DEI PARTECIPANTI ####
  }else if(myNodeId != 0){

    // Se l'asta è iniziata
    if(auctionStarted){
        // Se bottone premuto per fare offerta
        if(checkButtonPressed(BUTTON_BID_PIN)){
          triggerSendBid();                                                                      
        }
    }

  }
}

//----------------------------------------------------------------------------------------------------------------------------------------