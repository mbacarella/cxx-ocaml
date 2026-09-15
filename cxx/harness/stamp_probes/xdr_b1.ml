module M : sig type u end =
  struct module S = Set.Make (String) type v = A of S.t type u = int end
