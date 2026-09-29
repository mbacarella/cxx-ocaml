module N : sig module S : Set.S end = struct module S = Set.Make (String) end
