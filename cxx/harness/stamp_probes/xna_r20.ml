module F ( X : Set.OrderedType ) = struct module N = Set.Make( String ) module
  Mod : sig module XSet : Set.OrderedType end = struct module XSet = String end
  end
