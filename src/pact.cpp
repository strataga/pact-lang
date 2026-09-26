#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <charconv>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Support/raw_ostream.h>

namespace fs = std::filesystem;
#ifndef PACT_SANDBOX_RUNNER
#define PACT_SANDBOX_RUNNER ""
#endif
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

struct Diag { std::string code, message; };
struct Expr {
  enum Kind { Integer, Boolean, Variable, Binary, Compare, Conditional, Tool } kind;
  std::int64_t integer = 0;
  bool boolean = false;
  std::string text;
  char op = 0;
  std::unique_ptr<Expr> left, right, third;
};
using ExprPtr = std::unique_ptr<Expr>;
struct Stmt { bool isReturn = false; std::string name; ExprPtr value; };
struct Program { std::unordered_set<std::string> capabilities; std::vector<Stmt> body; };

struct Token { std::string value; std::size_t offset; };

class TextParser {
 public:
  explicit TextParser(const std::string &s) : source(s) { lex(); }
  std::optional<Program> parse() {
    Program p;
    while (!at("")) {
      if (accept("capability")) {
        std::string name = dottedName();
        if (!expect(";")) return std::nullopt;
        p.capabilities.insert(name);
      } else if (accept("let")) {
        Stmt s; s.name = identifier();
        if (!expect("=") || !(s.value = expression()) || !expect(";")) return std::nullopt;
        p.body.push_back(std::move(s));
      } else if (accept("return")) {
        Stmt s; s.isReturn = true; s.value = expression();
        if (!s.value || !expect(";")) return std::nullopt;
        p.body.push_back(std::move(s));
      } else { error("PACT-P001", "expected capability, let, or return"); return std::nullopt; }
    }
    return p;
  }
  std::vector<Diag> diagnostics;
 private:
  const std::string &source; std::vector<Token> tokens; std::size_t pos = 0;
  void lex() {
    for (std::size_t i=0; i<source.size();) {
      if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
      if (source[i]=='/' && i+1<source.size() && source[i+1]=='/') { while(i<source.size() && source[i]!='\n') ++i; continue; }
      std::size_t start=i;
      if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i]=='_') {
        while (i<source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i]=='_')) ++i;
      } else if (std::isdigit(static_cast<unsigned char>(source[i]))) {
        while (i<source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) ++i;
      } else if (source[i]=='=' && i+1<source.size() && source[i+1]=='=') i+=2;
      else ++i;
      tokens.push_back({source.substr(start,i-start),start});
    }
    tokens.push_back({"",source.size()});
  }
  const std::string &cur() const { return tokens[pos].value; }
  bool at(const std::string &s) const { return cur()==s; }
  bool accept(const std::string &s) { if(!at(s)) return false; ++pos; return true; }
  bool expect(const std::string &s) { if(accept(s)) return true; error("PACT-P002","expected '"+s+"'"); return false; }
  void error(std::string c,std::string m) { diagnostics.push_back({std::move(c),std::move(m)}); }
  std::string identifier() {
    if(cur().empty() || !(std::isalpha(static_cast<unsigned char>(cur()[0])) || cur()[0]=='_')) { error("PACT-P003","expected identifier"); return {}; }
    return tokens[pos++].value;
  }
  std::string dottedName() { std::string n=identifier(); if(accept(".")) n += "."+identifier(); return n; }
  ExprPtr expression(int minPrec=0, unsigned depth=0) {
    if (depth>=128) { error("PACT-P005","expression nesting exceeds maximum depth 128"); return {}; }
    ExprPtr lhs;
    if (accept("if")) {
      lhs=std::make_unique<Expr>(); lhs->kind=Expr::Conditional;
      lhs->left=expression(0,depth+1);
      if(!lhs->left || !expect("{") || !(lhs->right=expression(0,depth+1)) || !expect("}") || !expect("else") || !expect("{")) return {};
      lhs->third=expression(0,depth+1);
      if(!lhs->third || !expect("}")) return {};
    } else if (accept("tool")) {
      lhs=std::make_unique<Expr>(); lhs->kind=Expr::Tool; lhs->text=dottedName();
      if(!expect("(")) return {};
      lhs->left=expression(0,depth+1);
      if(!lhs->left || !expect(")")) return {};
    } else if (accept("(")) { lhs=expression(0,depth+1); if(!lhs || !expect(")")) return {}; }
    else if (accept("-")) {
      // The magnitude of INT64_MIN is one greater than INT64_MAX, so parse
      // this one signed literal before applying the ordinary i64 range check.
      if (cur()=="9223372036854775808") {
        ++pos; lhs=std::make_unique<Expr>(); lhs->kind=Expr::Integer;
        lhs->integer=std::numeric_limits<std::int64_t>::min();
      } else {
        auto x=expression(30,depth+1); if(!x) return {};
        auto z=std::make_unique<Expr>(); z->kind=Expr::Integer; z->integer=0;
        lhs=std::make_unique<Expr>(); lhs->kind=Expr::Binary; lhs->op='-';
        lhs->left=std::move(z); lhs->right=std::move(x);
      }
    }
    else if(!cur().empty() && std::isdigit(static_cast<unsigned char>(cur()[0]))) {
      std::int64_t n=0; const auto &text=cur(); auto parsed=std::from_chars(text.data(),text.data()+text.size(),n);
      if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) { error("PACT-P004","integer literal is outside i64 range"); return {}; }
      ++pos; lhs=std::make_unique<Expr>(); lhs->kind=Expr::Integer; lhs->integer=n;
    } else if (accept("true") || accept("false")) {
      lhs=std::make_unique<Expr>(); lhs->kind=Expr::Boolean; lhs->boolean=tokens[pos-1].value=="true";
    } else {
      auto n=identifier(); if(n.empty()) return {}; lhs=std::make_unique<Expr>(); lhs->kind=Expr::Variable; lhs->text=n;
    }
    while (true) {
      std::string token=cur(); char op=token.size()==1 ? token[0] : 0;
      int prec=token=="=="||op=='<'?5:(op=='+'||op=='-')?10:(op=='*'?20: -1);
      if(prec<minPrec) break;
      ++pos;
      auto rhs=expression(prec+1,depth+1);
      if(!rhs) return {};
      auto b=std::make_unique<Expr>(); b->kind=(token=="=="||op=='<')?Expr::Compare:Expr::Binary; b->op=token=="=="?'=':op; b->left=std::move(lhs); b->right=std::move(rhs); lhs=std::move(b);
    }
    return lhs;
  }
};

bool parseJsonExpr(const Value &v, ExprPtr &out, std::vector<Diag> &ds, unsigned depth=0) {
  if(depth>=128) { ds.push_back({"PACT-J010","expression nesting exceeds maximum depth 128"}); return false; }
  auto *o=v.getAsObject(); if(!o || o->empty()) { ds.push_back({"PACT-J001","expression must be a tagged object"}); return false; }
  if(auto value=o->getBoolean("bool")) { if(o->size()!=1) { ds.push_back({"PACT-J001","bool expression must contain exactly bool"}); return false; } out=std::make_unique<Expr>(); out->kind=Expr::Boolean; out->boolean=*value; return true; }
  if(auto n=o->getInteger("int")) { if(o->size()!=1) { ds.push_back({"PACT-J001","int expression must contain exactly int"}); return false; } out=std::make_unique<Expr>(); out->kind=Expr::Integer; out->integer=*n; return true; }
  if(auto n=o->getString("var")) { if(o->size()!=1 || n->empty()) { ds.push_back({"PACT-J001","var expression must contain exactly a non-empty var"}); return false; } out=std::make_unique<Expr>(); out->kind=Expr::Variable; out->text=*n; return true; }
  if(auto name=o->getString("tool")) {
    if(o->size()!=2) { ds.push_back({"PACT-J002","tool call must contain exactly tool and args"}); return false; }
    auto *args=o->getArray("args"); if(!args || args->size()!=1) { ds.push_back({"PACT-J002","tool call requires one argument"}); return false; }
    out=std::make_unique<Expr>(); out->kind=Expr::Tool; out->text=*name; return parseJsonExpr((*args)[0],out->left,ds,depth+1);
  }
  if(auto *b=o->getObject("bin")) {
    if(b->size()!=3 || o->size()!=1 || !b->getString("op") || !b->get("left") || !b->get("right")) { ds.push_back({"PACT-J003","bin must contain exactly op, left, and right"}); return false; }
    auto op=b->getString("op"); auto *l=b->get("left"), *r=b->get("right");
    if(!op || op->size()!=1 || ((*op)[0]!='+' && (*op)[0]!='-' && (*op)[0]!='*') || !l || !r) { ds.push_back({"PACT-J003","invalid binary expression"}); return false; }
    out=std::make_unique<Expr>(); out->kind=Expr::Binary; out->op=(*op)[0]; return parseJsonExpr(*l,out->left,ds,depth+1)&&parseJsonExpr(*r,out->right,ds,depth+1);
  }
  if(auto *cmp=o->getObject("cmp")) {
    if(cmp->size()!=3 || o->size()!=1 || !cmp->getString("op") || !cmp->get("left") || !cmp->get("right")) { ds.push_back({"PACT-J003","cmp must contain exactly op, left, and right"}); return false; }
    auto op=cmp->getString("op"); auto *l=cmp->get("left"), *r=cmp->get("right");
    if(!op || (*op!="==" && *op!="<") || !l || !r) { ds.push_back({"PACT-J003","invalid comparison expression"}); return false; }
    out=std::make_unique<Expr>(); out->kind=Expr::Compare; out->op=*op=="=="?'=':'<'; return parseJsonExpr(*l,out->left,ds,depth+1)&&parseJsonExpr(*r,out->right,ds,depth+1);
  }
  if(auto *conditional=o->getObject("if")) {
    if(conditional->size()!=3 || o->size()!=1 || !conditional->get("condition") || !conditional->get("then") || !conditional->get("else")) { ds.push_back({"PACT-J003","if must contain exactly condition, then, and else"}); return false; }
    out=std::make_unique<Expr>(); out->kind=Expr::Conditional;
    return parseJsonExpr(*conditional->get("condition"),out->left,ds,depth+1)&&parseJsonExpr(*conditional->get("then"),out->right,ds,depth+1)&&parseJsonExpr(*conditional->get("else"),out->third,ds,depth+1);
  }
  ds.push_back({"PACT-J004","unknown expression kind"}); return false;
}
std::optional<Program> parseJson(const std::string &s, std::vector<Diag> &ds) {
  auto v=llvm::json::parse(s); if(!v) { ds.push_back({"PACT-J000",llvm::toString(v.takeError())}); return std::nullopt; }
  auto *root=v->getAsObject(); auto *caps=root?root->getArray("capabilities"):nullptr; auto *body=root?root->getArray("body"):nullptr;
  if(!root || !caps || !body) { ds.push_back({"PACT-J005","program requires capabilities and body arrays"}); return std::nullopt; }
  // The original AST had no version field. Preserve it as v1 while allowing
  // producers to declare the contract explicitly and fail clearly on unknown
  // future formats instead of silently misinterpreting them.
  if (auto version = root->getInteger("version")) {
    if (*version != 1) {
      ds.push_back({"PACT-J011", "unsupported JSON AST version: " + std::to_string(*version)});
      return std::nullopt;
    }
  } else if (root->get("version")) {
    ds.push_back({"PACT-J011", "JSON AST version must be integer 1"});
    return std::nullopt;
  }
  Program p;
  for(const Value &c:*caps) { auto name=c.getAsString(); if(!name) { ds.push_back({"PACT-J006","capability must be a string"}); return std::nullopt; } p.capabilities.insert(name->str()); }
  for(const Value &item:*body) {
    auto *o=item.getAsObject(); if(!o) { ds.push_back({"PACT-J007","statement must be an object"}); return std::nullopt; }
    Stmt st;
    if(auto n=o->getString("let")) { auto *e=o->get("value"); if(o->size()!=2 || !e || n->empty()) { ds.push_back({"PACT-J008","let statement must contain exactly let and value"}); return std::nullopt; } st.name=n->str(); if(!parseJsonExpr(*e,st.value,ds)) return std::nullopt; }
    else if(auto *r=o->get("return")) { if(o->size()!=1) { ds.push_back({"PACT-J007","return statement must contain exactly return"}); return std::nullopt; } st.isReturn=true; if(!parseJsonExpr(*r,st.value,ds)) return std::nullopt; }
    else { ds.push_back({"PACT-J009","unknown statement kind"}); return std::nullopt; }
    p.body.push_back(std::move(st));
  }
  return p;
}

struct TypedInst { enum Op { Constant, BooleanConstant, Add, Subtract, Multiply, MathDouble, Equal, Less, Select } op; std::int64_t value=0; unsigned left=0,right=0,third=0; };
struct Checked { std::unordered_set<std::string> tools; std::vector<TypedInst> code; std::vector<bool> isBool; unsigned result=0; };
bool check(const Program &p,const std::unordered_set<std::string> &grants,Checked &c,std::vector<Diag> &ds) {
  std::unordered_map<std::string,unsigned> vars; bool returned=false;
  auto append=[&](TypedInst inst,bool isBool=false) { c.code.push_back(inst); c.isBool.push_back(isBool); return static_cast<unsigned>(c.code.size()-1); };
  auto containsTool=[&](auto&&self,const Expr &e)->bool {
    return e.kind==Expr::Tool || (e.left && self(self,*e.left)) || (e.right && self(self,*e.right)) || (e.third && self(self,*e.third));
  };
  auto lower=[&](auto&&self,const Expr&e)->unsigned {
    if(e.kind==Expr::Integer) return append({TypedInst::Constant,e.integer});
    if(e.kind==Expr::Boolean) return append({TypedInst::BooleanConstant,e.boolean?1:0},true);
    if(e.kind==Expr::Variable) { auto it=vars.find(e.text); if(it==vars.end()) { ds.push_back({"PACT-T001","unknown variable: "+e.text}); return 0; } return it->second; }
    if(e.kind==Expr::Tool) {
      if(e.text!="math.double") ds.push_back({"PACT-T002","unknown tool: "+e.text});
      if(!p.capabilities.count(e.text)) ds.push_back({"PACT-T003","source does not declare capability: "+e.text});
      if(!grants.count(e.text)) ds.push_back({"PACT-E003","host has not granted capability: "+e.text});
      unsigned arg=self(self,*e.left); if(arg<c.isBool.size() && c.isBool[arg]) ds.push_back({"PACT-T007","tool argument must be i64"});
      c.tools.insert(e.text); return append({TypedInst::MathDouble,0,arg});
    }
    if(e.kind==Expr::Conditional) {
      if(containsTool(containsTool,*e.right) || containsTool(containsTool,*e.third)) ds.push_back({"PACT-T013","tool calls are not allowed inside conditional branches"});
      unsigned condition=self(self,*e.left), thenValue=self(self,*e.right), elseValue=self(self,*e.third);
      if(condition<c.isBool.size() && !c.isBool[condition]) ds.push_back({"PACT-T008","conditional condition must be bool"});
      if(thenValue<c.isBool.size() && c.isBool[thenValue]) ds.push_back({"PACT-T009","conditional branches must be i64"});
      if(elseValue<c.isBool.size() && c.isBool[elseValue]) ds.push_back({"PACT-T009","conditional branches must be i64"});
      return append({TypedInst::Select,0,condition,thenValue,elseValue});
    }
    unsigned left=self(self,*e.left),right=self(self,*e.right);
    if(e.kind==Expr::Compare) {
      if((left<c.isBool.size() && c.isBool[left]) || (right<c.isBool.size() && c.isBool[right])) ds.push_back({"PACT-T010","comparison operands must be i64"});
      return append({e.op=='='?TypedInst::Equal:TypedInst::Less,0,left,right},true);
    }
    if((left<c.isBool.size() && c.isBool[left]) || (right<c.isBool.size() && c.isBool[right])) ds.push_back({"PACT-T011","arithmetic operands must be i64"});
    auto op=e.op=='+'?TypedInst::Add:e.op=='-'?TypedInst::Subtract:TypedInst::Multiply;
    return append({op,0,left,right});
  };
  for(std::size_t i=0;i<p.body.size();++i) {
    const auto &s=p.body[i]; if(returned) { ds.push_back({"PACT-T004","return must be the final statement"}); break; }
    unsigned value=lower(lower,*s.value);
    if(s.isReturn) { returned=true;c.result=value; if(value<c.isBool.size() && c.isBool[value]) ds.push_back({"PACT-T012","return value must be i64"}); }
    else if(vars.count(s.name)) ds.push_back({"PACT-T005","duplicate binding: "+s.name});
    else vars.emplace(s.name,value);
  }
  if(!returned) ds.push_back({"PACT-T006","program must return a value"});
  return ds.empty();
}

bool emitIR(const Checked &c,const fs::path &path,std::string &error) {
  llvm::LLVMContext ctx; auto m=std::make_unique<llvm::Module>("pact.module",ctx); m->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  auto *i64=llvm::Type::getInt64Ty(ctx);
  auto *i1=llvm::Type::getInt1Ty(ctx);
  if(c.tools.count("math.double")) {
    auto *fty=llvm::FunctionType::get(i64,{i64},false); llvm::Function::Create(fty,llvm::Function::ExternalLinkage,"pact_math_double",m.get());
  }
  auto *mainTy=llvm::FunctionType::get(i64,false); auto *fn=llvm::Function::Create(mainTy,llvm::Function::ExternalLinkage,"pact_entry",m.get());
  auto *bb=llvm::BasicBlock::Create(ctx,"entry",fn); llvm::IRBuilder<> b(bb); std::vector<llvm::Value*> values;
  for(const auto &i:c.code) { llvm::Value *v=nullptr; switch(i.op) {
    case TypedInst::Constant: v=llvm::ConstantInt::getSigned(i64,i.value); break;
    case TypedInst::BooleanConstant: v=llvm::ConstantInt::get(i1,i.value!=0); break;
    case TypedInst::Add: v=b.CreateAdd(values.at(i.left),values.at(i.right),"add"); break;
    case TypedInst::Subtract: v=b.CreateSub(values.at(i.left),values.at(i.right),"sub"); break;
    case TypedInst::Multiply: v=b.CreateMul(values.at(i.left),values.at(i.right),"mul"); break;
    case TypedInst::MathDouble: v=b.CreateCall(m->getFunction("pact_math_double"),{values.at(i.left)},"tool.call"); break;
    case TypedInst::Equal: v=b.CreateICmpEQ(values.at(i.left),values.at(i.right),"cmp.eq"); break;
    case TypedInst::Less: v=b.CreateICmpSLT(values.at(i.left),values.at(i.right),"cmp.lt"); break;
    case TypedInst::Select: v=b.CreateSelect(values.at(i.left),values.at(i.right),values.at(i.third),"if.value"); break;
  } values.push_back(v); }
  b.CreateRet(values.at(c.result));
  if(llvm::verifyModule(*m,&llvm::errs())) { error="LLVM rejected generated module"; return false; }
  std::error_code ec; llvm::raw_fd_ostream out(path.string(),ec); if(ec) { error=ec.message(); return false; } m->print(out,nullptr); return true;
}
struct ChildStatus { int code=127; int signal=0; };
ChildStatus spawn(const std::vector<std::string> &args,const std::string &path="/usr/bin:/bin",const std::string &capture={}) {
  std::vector<char*> av; for(const auto &a:args) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
  pid_t p=fork(); if(p<0) return {127,0};
  if(p==0) {
    setenv("PATH",path.c_str(),1);
    if(!capture.empty()){int fd=open(capture.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)_exit(126);dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);}
    execvp(av[0],av.data()); _exit(127);
  }
  int status=0; while(waitpid(p,&status,0)<0) if(errno!=EINTR) return {127,0};
  if(WIFEXITED(status)) return {WEXITSTATUS(status),0};
  if(WIFSIGNALED(status)) return {0,WTERMSIG(status)};
  return {127,0};
}
struct PrivateTempDir {
  fs::path path;
  bool create() {
    std::string pattern=(fs::temp_directory_path()/"pact-XXXXXX").string();
    char *created=::mkdtemp(pattern.data());
    if(!created) return false;
    path=created; return true;
  }
  void cleanup() noexcept { if(!path.empty()) { std::error_code ec; fs::remove_all(path,ec); path.clear(); } }
  ~PrivateTempDir() { cleanup(); }
};
static std::string sandboxRunnerPath(const char *argv0) {
  if(std::string(PACT_SANDBOX_RUNNER).size()) return PACT_SANDBOX_RUNNER;
  std::error_code ec;
  fs::path exe=fs::read_symlink("/proc/self/exe",ec);
  if(ec) exe=fs::absolute(argv0,ec);
  if(ec) return "";
  return (exe.parent_path()/"runtime"/"sandbox_runner.py").string();
}
void report(const std::vector<Diag> &ds,bool ok) {
  Object root; root["diagnostic_version"]=1; root["ok"]=ok; Array a; for(const auto &d:ds) { Object o; o["code"]=d.code; o["message"]=d.message; a.push_back(std::move(o)); } root["diagnostics"]=std::move(a); llvm::outs()<<Value(std::move(root))<<"\n";
}
int main(int argc,char **argv) {
  if(argc<3) { std::cerr<<"usage: pact check|compile|run [--allow CAP] [-o FILE] INPUT\n"; return 2; }
  std::string command=argv[1], output; std::unordered_set<std::string> grants; std::string input;
  for(int i=2;i<argc;++i) { std::string a=argv[i]; if(a=="--allow"&&i+1<argc) grants.insert(argv[++i]); else if(a=="-o"&&i+1<argc) output=argv[++i]; else input=a; }
  if(input.empty()) { std::cerr<<"missing input file\n"; return 2; }
  std::ifstream in(input); if(!in) { report({{"PACT-I001","cannot read input file"}},false); return 1; }
  std::string source((std::istreambuf_iterator<char>(in)),{}); std::vector<Diag> ds; std::optional<Program> p;
  if(fs::path(input).extension()==".json") p=parseJson(source,ds);
  else { TextParser parser(source); p=parser.parse(); ds=parser.diagnostics; }
  if(!p) { report(ds,false); return 1; }
  Checked checked; check(*p,grants,checked,ds); if(!ds.empty()) { report(ds,false); return 1; }
  if(command=="check") { report({},true); return 0; }
  if(command!="compile"&&command!="run") { std::cerr<<"unknown command\n"; return 2; }
  bool temporary=command=="run";
  if(!temporary && output.empty()) { std::cerr<<"compile requires -o FILE\n"; return 2; }
  PrivateTempDir temp;
  if(!temp.create()) { report({{"PACT-C003","could not create private temporary directory"}},false); return 1; }
  if(temporary) output=(temp.path/"program").string();
  fs::path ir=temp.path/"module.ll"; std::string error;
  if(!emitIR(checked,ir,error)) { report({{"PACT-C001",error}},false); return 1; }
  ChildStatus link=spawn({"clang++","-Wno-override-module","-x","ir",ir.string(),"-x","c++",PACT_RUNTIME_SOURCE,"-o",output});
  if(link.signal) { report({{"PACT-C002","native linker terminated by signal "+std::to_string(link.signal)}},false); return 1; }
  if(link.code) { report({{"PACT-C002","native link failed (clang++ exit "+std::to_string(link.code)+")"}},false); return 1; }
  if(command=="compile") { report({},true); return 0; }
  fs::path runStatus=temp.path/"run.status";
  const std::string runnerPath=sandboxRunnerPath(argv[0]);
  std::error_code runnerEc;
  if(runnerPath.empty()||!fs::is_regular_file(runnerPath,runnerEc)||runnerEc) {
    report({{"PACT-S004","secure sandbox launcher is unavailable; refusing direct execution"}},false);return 1;
  }
  fs::path runOutput=temp.path/"program.output";
  ChildStatus runner=spawn({"python3",runnerPath,output,runStatus.string(),std::to_string(getpid())},"/usr/bin:/bin",runOutput.string());
  std::ifstream statusFile(runStatus);
  std::string status;
  if(!std::getline(statusFile,status)) {
    report({{"PACT-S001","sandbox runner did not report an execution status"}},false);
    return 1;
  }
  if(status=="timeout") {
    report({{"PACT-S002","program exceeded the 30 second sandbox wall-time limit"}},false);
    return 1;
  }
  if(status=="output_limit") {
    report({{"PACT-S003","program exceeded the 1 MiB combined output limit"}},false);
    return 1;
  }
  if(status=="cpu_limit") {
    report({{"PACT-S005","program exceeded the 5 second aggregate CPU limit"}},false);
    return 1;
  }
  if(status=="cancelled") {
    report({{"PACT-S006","sandbox execution was cancelled and its process scope was stopped"}},false);
    return 1;
  }
  if(status=="cleanup_failed") {
    report({{"PACT-S008","sandbox process scope could not be confirmed empty"}},false);
    return 1;
  }
  if(status.rfind("sandbox_unavailable:",0)==0) {
    report({{"PACT-S004",status.substr(std::string("sandbox_unavailable:").size())}},false);
    return 1;
  }
  if(status.rfind("ok:",0)!=0) {
    report({{"PACT-S001","sandbox runner returned an invalid execution status"}},false);
    return 1;
  }
  int programCode=127;
  const std::string codeText=status.substr(3);
  auto parsedCode=std::from_chars(codeText.data(),codeText.data()+codeText.size(),programCode);
  if(parsedCode.ec!=std::errc{} || parsedCode.ptr!=codeText.data()+codeText.size()) {
    report({{"PACT-S001","sandbox runner returned an invalid program exit code"}},false);
    return 1;
  }
  if(programCode==126) {
    report({{"PACT-S007","runtime syscall filter could not be installed"}},false);
    return 1;
  }
  if(programCode==125) {
    report({{"PACT-S009","host tool broker request failed or returned an invalid response"}},false);
    return 1;
  }
  if(runner.signal || runner.code!=0) {
    report({{"PACT-S001","sandbox runner itself failed"}},false);
    return 1;
  }
  std::ifstream outputFile(runOutput,std::ios::binary);
  if(!outputFile){report({{"PACT-S003","sandbox output capture was unavailable"}},false);return 1;}
  std::string captured((std::istreambuf_iterator<char>(outputFile)),{});
  if(outputFile.bad()||captured.size()>1024*1024) {
    report({{"PACT-S003","sandbox output capture was unavailable or exceeded its bound"}},false);return 1;
  }
  if(programCode!=0 && (captured.find("bwrap:")!=std::string::npos||captured.find("systemd-run:")!=std::string::npos||captured.find("Failed to start transient scope")!=std::string::npos||captured.find("Failed to connect to bus")!=std::string::npos||captured.find("Failed to set unit properties")!=std::string::npos)) {
    std::string why=captured.substr(0,1024);
    report({{"PACT-S004","sandbox namespace or cgroup setup failed; no direct execution fallback was attempted: "+why}},false);return 1;
  }
  std::cout.write(captured.data(),static_cast<std::streamsize>(captured.size()));
  return programCode;
}
