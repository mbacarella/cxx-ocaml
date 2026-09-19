module F ( X : Set.OrderedType ) = struct module rec Mod : sig module XSet :
  Set.S end = struct module XSet = Set.Make( X ) end and N : sig end = struct
  end end
