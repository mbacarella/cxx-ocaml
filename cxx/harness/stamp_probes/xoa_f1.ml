(* TEST *)

open Printf
open Effect
open Effect.Deep

(* Francois Pottier's implementation of shallow handlers on top of
   deep handlers, by reification of an effectful operation
   as a stream of events. *)

module MkReify
  (X : sig
     (* A type of operations ['a op]. *)
     type 'a op
     (* An effect name [E]. *)
     type _ eff += E : 'a op -> 'a eff
  end)
= struct
  open Effect
  open Effect.Deep
  open X

  (* The type ['a event] represents a computation whose result type is ['a].
     It can be thought of as a lazy sequence of events, where an event is
     either normal termination [Ret] or an effect [Eff]. The first event of
     the stream is immediately available for inspection; the rest of the
     computation is suspended and represented as a continuation. *)

  type 'a event =
  | Ret : 'a -> 'a event
  | Eff : 'a op * ('a, 'b event) continuation -> 'b event

  (* [reify] transforms an effectful computation into a stream of events.
     The effects named [E] are caught and become events in the stream. *)

  let reify (type a) (m : unit -> a) : a event =
    match m () with
    | x -> Ret x
    | effect E op, k -> Eff(op, k)

end

module PC = struct

  type data = int

  type _ op =
    | Yield : data -> unit op
    | Await : data op

  type _ eff += E : 'a op -> 'a eff

end

open PC

let yield x =
  perform (E (Yield x))

let await () =
  perform (E Await)

exception ProducerPushedTooFar (* This helps us test. *)

let zero_producer () =
  raise ProducerPushedTooFar

let zero_consumer () =
  "I need no data."

let test_producer () =
  yield 1;
  yield 2;
  raise ProducerPushedTooFar

let test_consumer () =
  let x = await() in
  let y = await() in
  Printf.sprintf "I have received %d and %d." x y

open MkReify(PC)
let a = Ret 1
let b (c : int event) = match c with Eff (_, _) -> 1 | _ -> 0
