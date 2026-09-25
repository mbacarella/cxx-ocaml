module type S = sig open Map.Make(String) type 'a u = 'a t val f : key -> int u end
