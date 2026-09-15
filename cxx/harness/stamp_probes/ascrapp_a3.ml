module M : sig val y : int end =
  struct module S = Set.Make(String) let y = 0 end
