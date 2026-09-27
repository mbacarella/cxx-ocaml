module F : functor (X : sig val x : int end) -> sig end = functor (X : sig val x : string end) -> struct end
