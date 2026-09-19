module M : sig module A : Set.S module B : Set.S end = struct module A =
  Set.Make( String ) module B = Set.Make( Bool ) end
