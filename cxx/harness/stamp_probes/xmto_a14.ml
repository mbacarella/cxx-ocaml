type z = int
module Hash1 : sig include (module type of Hashtbl) end = Hashtbl
module Hash2 : sig include (module type of Hashtbl) end = Hashtbl
