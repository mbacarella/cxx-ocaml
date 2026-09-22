open Printf
open Effect
open Effect.Deep
module MkReify
  (X : sig
     type 'a op
  end)
= struct
  type 'a event =
  | Ret : 'a -> 'a event
end
type r2 = Done2
