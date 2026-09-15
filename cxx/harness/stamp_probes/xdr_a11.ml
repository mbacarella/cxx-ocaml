module M : sig type u end =
  struct module S = Hashtbl.Make (String) type u = int S.t end
