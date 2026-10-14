(* FUZZ: levels=byte,nat,fl
   Upstream bug 5 in cxx/UPSTREAM-BUGS.md (ocaml/ocaml#15160, PRs #15161
   and #15162): Cmm_helpers.array_indexing and make_checkbound compute
   constants with the compiler's 63-bit int, which wrap.  A checked
   a.(2^60 + 1) on a 4-element array raises in bytecode but natively
   passes a bounds check of [hdr <=u 2047] and reads a.(1).  The other
   functions are compiled (their Cmm is compared) but never called: they
   segfault natively.  The port reproduces all of it (stock is the
   reference). *)
open Bigarray
let k = 0x1000_0000_0000_0000 (* 2^60 *)
(* #15162: (m lsl 9) wraps in make_checkbound, (i lsl 3) in array_indexing *)
let[@inline never] get (a : int array) = a.(0x1000_0000_0000_0001)
let[@inline never] getf (a : float array) = a.(0x1000_0000_0000_0001)
let[@inline never] set (a : int array) v = a.(0x1000_0000_0000_0001) <- v
let[@inline never] local n =
  let a = Array.make 4 n in a.(1) <- 2; a.(0x1000_0000_0000_0001)
(* #15160 (1): (n-1) lsl (log2size-1) wraps although i - k is small *)
let[@inline never] u_off (a : int array) i = Array.unsafe_get a (i - k)
let[@inline never] u_offf (a : float array) i = Array.unsafe_get a (i - k)
let[@inline never] u_off32 (b : (int32, int32_elt, c_layout) Array1.t) i =
  Array1.unsafe_get b (i - k)
(* #15160 (2): 1-byte elements, the address uses c, the index untag (tag c) *)
let[@inline never] u_char (b : (char, int8_unsigned_elt, c_layout) Array1.t) x =
  Array1.unsafe_get b (Int64.to_int x)
let show f =
  match f () with v -> v | exception Invalid_argument _ -> "Invalid_argument"
let () =
  print_endline (show (fun () -> string_of_int (get [| 10; 11; 12; 13 |])));
  print_endline (show (fun () -> string_of_float (getf [| 1.; 2.; 3.; 4. |])));
  print_endline (show (fun () ->
    let a = [| 10; 11; 12; 13 |] in set a 42; string_of_int a.(1)));
  print_endline (show (fun () -> string_of_int (local (Sys.opaque_identity 7))))
