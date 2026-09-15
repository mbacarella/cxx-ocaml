module N : sig type u end =
  struct module S = Set.Make (String) type u = S.t end
