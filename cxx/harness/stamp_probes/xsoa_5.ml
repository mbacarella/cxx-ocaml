module type S = sig open Hashtbl.Make(String) type v = int t val k : key end
