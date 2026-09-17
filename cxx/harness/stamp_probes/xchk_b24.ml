module X = (struct
    module F (_ : sig end) = struct end
  end : sig module F : functor (S : sig end) -> sig end end)
