type z = int
module type T = module type of Hashtbl
module Hash1 : module type of Hashtbl = Hashtbl
