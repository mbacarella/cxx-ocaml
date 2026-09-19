module W : sig module F ( X : Set.OrderedType ) : sig type u end end = struct
  module F ( X : Set.OrderedType ) = struct type u = Set.Make( X ).t end end
