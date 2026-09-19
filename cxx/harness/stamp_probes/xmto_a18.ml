type z = int
module Hash1 : module type of Hashtbl = Hashtbl
module Hash2 : sig include (module type of Hashtbl) end = Hashtbl
module Hash3 : sig include (module type of Hashtbl) end = Hashtbl
