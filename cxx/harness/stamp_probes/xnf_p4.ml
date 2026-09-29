module F ( X : Set.OrderedType ) = struct module V : sig module G ( Y :
  Set.OrderedType ) : sig type u = Set.Make( Y ).t end end = struct module G ( Y
  : Set.OrderedType ) = struct type u = Set.Make( Y ).t end end end
