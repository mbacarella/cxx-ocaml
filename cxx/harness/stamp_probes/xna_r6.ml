module A : Set.S = Set.Make( String ) module M : sig module XSet : Set.S end =
  struct module XSet = Set.Make( String ) end
