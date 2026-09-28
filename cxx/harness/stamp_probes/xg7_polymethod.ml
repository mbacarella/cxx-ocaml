(* G7: a method `m : type a. ...` is typed through Pexp_poly with the
   constraint copied by Ast_helper.Typ.varify_constructors, which shares
   the original's strings, locations and attributes (ppxlib's
   Attribute.get_internal). *)
module M = struct type 'a t = A of 'a | B end
class o =
  object
    method m : type a. a M.t -> a option = function M.A x -> Some x | M.B -> None
    method private p : type b. b list -> b M.t = function x :: _ -> M.A x | [] -> M.B
  end
