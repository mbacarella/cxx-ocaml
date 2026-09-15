module N : sig val y : int end =
  struct module S = Map.Make (String) let y = 0 end
