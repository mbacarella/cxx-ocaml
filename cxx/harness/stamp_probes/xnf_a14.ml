module W = struct type u = Set.Make( String ).t end module Z : sig end = struct
  type v = Set.Make( String ).t end
