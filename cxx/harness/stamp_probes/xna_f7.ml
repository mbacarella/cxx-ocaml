module M : sig module XSet : sig type t end end = struct module Y = Set.Make(
  Bool ) module XSet = Set.Make( String ) end
