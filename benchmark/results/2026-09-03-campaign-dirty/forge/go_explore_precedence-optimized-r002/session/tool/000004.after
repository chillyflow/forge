package config

func Resolve(defaults, file, overrides Values) Values {
	out := Values{}
	for k, v := range defaults {
		out[k] = v
	}
	for k, v := range file {
		out[k] = v
	}
	for k, v := range overrides {
		out[k] = v
	}
	return out
}
