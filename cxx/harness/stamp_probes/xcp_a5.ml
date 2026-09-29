open Printf
open Effect
open Effect.Deep
module MkReify
  (X : sig
     type 'a op
     type _ eff += E : 'a op -> 'a eff
  end)
= struct
  open Effect
  open Effect.Deep
  open X
  type 'a event =
  | Ret : 'a -> 'a event
  | Eff : 'a op * ('a, 'b event) continuation -> 'b event
end
type r2 = Done2
