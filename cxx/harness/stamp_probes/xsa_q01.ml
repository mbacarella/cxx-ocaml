(* TEST *)

(** This test weak table by application to the memoization of collatz
    (also known as syracuse) algorithm suite computation *)

(** We use Int64 because they are boxed *)

(** number of element of the suite to compute (more are computed) *)
let n = 1000

let two = Int64.of_int 2
let three = Int64.of_int 3

let collatz x =
  if Int64.equal (Int64.rem x two) Int64.zero
  then Int64.div x two
  else Int64.succ (Int64.mul x three)

module S = struct
  include Int64
  let hash (x:t) = Hashtbl.hash x
end

let pp = Int64.to_string

module HW = Ephemeron.K1.Make(S)
module SW = Weak.Make(S)
