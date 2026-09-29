(** A module exercising most of the parsetree, run through the identity
    rewriter. *)

type 'a tree = Leaf | Node of 'a tree * 'a * 'a tree  (** a binary tree *)
type point = { x : float; mutable y : float } [@@warning "-69"]
type _ expr = Int : int -> int expr | Add : int expr * int expr -> int expr

exception Stop of string [@@deprecated "no"]

let rec size = function Leaf -> 0 | Node (l, _, r) -> size l + 1 + size r
let rec eval : type a. a expr -> a = function Int n -> n | Add (a, b) -> eval a + eval b

module type S = sig
  type t
  val v : t
end

module F (X : S) : S with type t = X.t list = struct
  type t = X.t list
  let v = [ X.v ]
end

module I = F (struct type t = int let v = 3 end)

class counter init = object (self)
  val mutable n = init
  method incr = n <- n + 1; self
  method get = n
end

let poly (`A | `B as v) = match v with `A -> 1 | `B -> 2
let labels ~a ?(b = 2) () = a + b
let lets =
  let ( let* ) x f = Option.bind x f in
  let* a = Some 1 in
  let* b = Some 2 in
  Some (a + b)
let objs = (new counter 3)#incr#get
let pack = (module I : S with type t = int list)
let local = let module M = struct let z = 1 end in M.z
let arrays = [| 1; 2 |].(0) + String.length "s"
let tuple = (1, "a", 'c', 1.5, 3l, 4L, 5n)
let f = fun x -> x [@inline]
let () = assert (size (Node (Leaf, 1, Leaf)) = 1)
let _ = try raise (Stop "x") with Stop _ -> ()
let seq = for i = 0 to 1 do ignore i done; while false do () done
let lazyv = lazy (1 + 2)
let recd = let p = { x = 1.; y = 2. } in { p with y = 3. }
