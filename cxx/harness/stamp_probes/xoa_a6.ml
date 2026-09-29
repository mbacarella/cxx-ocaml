module F (X : sig type 'a op type _ eff += E : 'a op -> 'a eff end) = struct
  open X
  type 'a event =
    | Ret : 'a -> 'a event
    | Eff : 'a op * ('a, 'b event) Effect.Deep.continuation -> 'b event
  let reify (type a) (m : unit -> a) : a event =
    match m () with x -> Ret x | effect E op, k -> Eff(op, k)
end
module PC = struct
  type data = int
  type _ op = Yield : data -> unit op | Await : data op
  type _ eff += E : 'a op -> 'a eff
end
module Q = F(PC)
