// Kenyan towns for the weather tab (approximate town-centre coordinates).
const KENYA_TOWNS = [
  ['Nairobi', -1.2864, 36.8172], ['Mombasa', -4.0435, 39.6682], ['Kisumu', -0.0917, 34.7680],
  ['Nakuru', -0.3031, 36.0800], ['Eldoret', 0.5143, 35.2698], ['Thika', -1.0333, 37.0693],
  ['Naivasha', -0.7167, 36.4310], ['Nyeri', -0.4201, 36.9476], ['Nanyuki', 0.0167, 37.0740],
  ['Nyahururu', 0.0380, 36.3640], ['Meru', 0.0470, 37.6490], ['Embu', -0.5310, 37.4500],
  ['Machakos', -1.5177, 37.2634], ['Kitui', -1.3670, 38.0110], ['Makindu', -2.2780, 37.8260],
  ['Mtito Andei', -2.6900, 38.1660], ['Voi', -3.3960, 38.5560], ['Taveta', -3.3980, 37.6760],
  ['Kajiado', -1.8520, 36.7760], ['Namanga', -2.5450, 36.7870], ['Narok', -1.0780, 35.8610],
  ['Kericho', -0.3680, 35.2830], ['Kisii', -0.6820, 34.7670], ['Kakamega', 0.2830, 34.7520],
  ['Bungoma', 0.5630, 34.5610], ['Busia', 0.4600, 34.1110], ['Kitale', 1.0160, 35.0060],
  ['Homa Bay', -0.5270, 34.4570], ['Migori', -1.0630, 34.4730], ['Maralal', 1.0970, 36.6980],
  ['Isiolo', 0.3540, 37.5820], ['Marsabit', 2.3340, 37.9900], ['Moyale', 3.5270, 39.0560],
  ['Lodwar', 3.1190, 35.5970], ['Garissa', -0.4530, 39.6460], ['Wajir', 1.7470, 40.0570],
  ['Mandera', 3.9370, 41.8560], ['Malindi', -3.2190, 40.1170], ['Kilifi', -3.6310, 39.8490],
  ['Diani', -4.3160, 39.5780], ['Lamu', -2.2720, 40.9020],
].map(([name, lat, lon]) => ({ name, lat, lon }));

// WMO weather codes used by Open-Meteo
const WMO = {
  0: 'Clear', 1: 'Mainly clear', 2: 'Partly cloudy', 3: 'Overcast', 45: 'Fog', 48: 'Rime fog',
  51: 'Light drizzle', 53: 'Drizzle', 55: 'Heavy drizzle', 56: 'Freezing drizzle', 57: 'Freezing drizzle',
  61: 'Light rain', 63: 'Rain', 65: 'Heavy rain', 66: 'Freezing rain', 67: 'Freezing rain',
  71: 'Light snow', 73: 'Snow', 75: 'Heavy snow', 77: 'Snow grains',
  80: 'Light showers', 81: 'Showers', 82: 'Violent showers', 85: 'Snow showers', 86: 'Snow showers',
  95: 'Thunderstorm', 96: 'Thunderstorm, hail', 99: 'Thunderstorm, heavy hail',
};
