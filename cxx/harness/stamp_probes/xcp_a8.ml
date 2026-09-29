module MkReify (X : sig type 'a op end) = struct
  type 'a event = Ret : 'a -> 'a event
end
type r2 = Done2
