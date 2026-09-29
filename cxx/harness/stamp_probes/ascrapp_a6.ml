module M : sig end =
  struct module S = Set.Make(String) module T = Hashtbl.Make(String) end
