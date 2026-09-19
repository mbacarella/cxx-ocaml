type z = int
module Hash1 : module type of Hashtbl = Hashtbl
module type T = module type of Hashtbl
