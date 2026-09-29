module M : sig module XSet : Set.S with type elt = string end = struct module
  XSet = Set.Make( String ) end
