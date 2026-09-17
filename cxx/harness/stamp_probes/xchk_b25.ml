module X = (struct
    module F (S : sig end) = struct end
  end : sig module F : functor (S : sig end) -> sig end end)
