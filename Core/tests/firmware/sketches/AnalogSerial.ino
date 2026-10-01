// Arduino "AnalogReadSerial" + millis(): prints "<millis> <A0>" every 50 ms at 115200 baud and fades pin 9 with A0.
void setup() { Serial.begin(115200); pinMode(9, OUTPUT); }
void loop() {
  int v = analogRead(A0);
  analogWrite(9, v / 4);
  Serial.print(millis());
  Serial.print(' ');
  Serial.println(v);
  delay(50);
}
