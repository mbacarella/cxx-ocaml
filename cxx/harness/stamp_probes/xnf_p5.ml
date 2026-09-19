module W : sig module G ( Y : Set.OrderedType ) : sig type u = Set.Make( Y ).t
  end end = struct module G ( Y : Set.OrderedType ) = struct type u = Set.Make(
  Y ).t end end
