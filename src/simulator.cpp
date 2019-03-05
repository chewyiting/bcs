//----------------------------------------------------------
// Copyright 2017 University of Oxford
// Written by Michael A. Boemo (michael.boemo@path.ox.ac.uk)
// This software is licensed under GPL-2.0.  You should have
// received a copy of the license with this software.  If
// not, please Email the author.
//----------------------------------------------------------

//#define DEBUG 1

#include <functional>
#include <cassert>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include "blockParser.h"
#include "error_handling.h"
#include "simulator.h"
#include "evaluate_trees.h"

System::System( std::list< SystemProcess > &s, std::map< std::string, ProcessDefinition > &processDefs, int mT, double mD, GlobalVariables &globalVars ){

	_name2ProcessDef = processDefs;
	_maxTransitions = mT;
	_maxDuration = mD;
	_globalVars = globalVars;

	for ( auto i = s.begin(); i != s.end(); i++ ){

		_currentProcesses.push_back( new SystemProcess( *i ) );
	}

	//do an initial pass through the whole system
	std::list< SystemProcess * > newProcesses;
	for ( auto sp = _currentProcesses.begin(); sp != _currentProcesses.end(); sp++ ){

		//see if we can make multiple system processes out of this one by splitting on parallel operators
		if ( ((*sp) -> parseTree).getRoot() -> identify() == "Parallel" ){

			splitOnParallel( *sp, ((*sp) -> parseTree).getRoot(), newProcesses );
			delete *sp;
			sp = _currentProcesses.erase( sp );
		}
	}
	_currentProcesses.insert( _currentProcesses.end(), newProcesses.begin(), newProcesses.end() );

	//sum the transition rates for non-handshake candidates while buildling a list of handshake candidates
	std::list< SystemProcess > parallelProcesses;
	for ( auto s = _currentProcesses.begin(); s != _currentProcesses.end(); s++ ){

		sumTransitionRates( *s, (*s) -> parseTree, ((*s) -> parseTree).getRoot(), parallelProcesses, (*s) -> parameterValues );
	}

	//sum handshake transitions
	for ( auto chan = _handshakes_Name2Channel.begin(); chan != _handshakes_Name2Channel.end(); chan++ ){

		int newHandshakesAdded;
		double rateSumIncrease;
		std::tie(newHandshakesAdded,rateSumIncrease) = (chan -> second) -> updateHandshakeCandidates();
		_candidatesLeft += newHandshakesAdded;
		_rateSum += rateSumIncrease;
	}
}


void System::writeTransition( double time, std::shared_ptr<Candidate> chosen, std::stringstream &ss ){

	Block *actionDone = chosen -> actionCandidate;
	std::vector< std::string > parameterNames = _name2ProcessDef[ actionDone -> getOwningProcess()].parameters;

	if ( actionDone -> identify() == "Action" ){

		ActionBlock *ab = dynamic_cast< ActionBlock * >( actionDone );
		ss << time << '\t' << ab -> actionName << '\t' << actionDone -> getOwningProcess();
	}
	else if ( actionDone -> identify() == "MessageSend" ){

		MessageSendBlock *msb = dynamic_cast< MessageSendBlock * >( actionDone );
		ss << time << '\t' << msb -> getChannelName() << '\t' << actionDone -> getOwningProcess();
	}
	else if ( actionDone -> identify() == "MessageReceive" ){

		MessageReceiveBlock *mrb = dynamic_cast< MessageReceiveBlock * >( actionDone );
		ss << time << '\t' << mrb -> getChannelName()  << '\t' << actionDone -> getOwningProcess();
	}

	for ( auto p = parameterNames.begin(); p < parameterNames.end(); p++ ){

		if ( (chosen -> parameterValues).intValues.count(*p) > 0 ){
			ss << '\t' << *p << '\t' << (chosen -> parameterValues).intValues[*p];
		}
		else{
			ss << '\t' << *p << '\t' << (chosen -> parameterValues).doubleValues[*p];
		}
	}
	ss << std::endl;
}


std::string substituteChannelName( std::string channelName, std::map< std::string, int > &parameterValues ){

	if ( parameterValues.find( channelName ) != parameterValues.end() ){

		return std::to_string( parameterValues[channelName] );
	}
	else return channelName;
}


void System::sumTransitionRates( SystemProcess *sp,
			 Tree<Block> &bt,
			 Block *current,
			 std::list< SystemProcess > parallelProcesses,
			 ParameterValues currentParameters ){

	if ( current -> identify() == "Action" ){

		double rate = evalRPN_double( current -> getRate(), currentParameters, _globalVars, sp -> localVariables );
		if ( rate <= 0 ) throw BadRate( current -> getToken() );
		std::shared_ptr<Candidate> cand( new Candidate( current, currentParameters, sp -> localVariables, sp, parallelProcesses ) );
		cand -> rate = rate;
		_nonMsgCandidates[sp].push_back( cand );
		_candidatesLeft++;
		_rateSum += rate;
	}
	else if ( current -> identify() == "MessageSend" ){

		MessageSendBlock *msb = dynamic_cast< MessageSendBlock * >( current );
		std::string channelName = substituteChannelName( msb -> getChannelName(), currentParameters.intValues );

		if ( msb -> isHandshake() ){

			double rate = evalRPN_double( msb -> getRate(), currentParameters, _globalVars, sp -> localVariables );
			if ( rate <= 0 ) throw BadRate( current -> getToken() );

			std::shared_ptr< Candidate > cand( new Candidate( msb, currentParameters, sp -> localVariables, sp, parallelProcesses) );

			cand -> rate = rate;
			int paramEval = evalRPN_int( msb -> getParameterExpression(), currentParameters, _globalVars, sp -> localVariables );
			(cand -> rangeEvaluation).insert( paramEval );

			if ( _handshakes_Name2Channel.find( channelName ) != _handshakes_Name2Channel.end() ){

				_handshakes_Name2Channel[channelName] -> addSendCandidate(cand);
			}
			else{
				std::shared_ptr< HandshakeChannel > newChannel(new HandshakeChannel(channelName, _globalVars));
				_handshakes_Name2Channel[channelName] = newChannel;
				_handshakes_Name2Channel[channelName] -> addSendCandidate(cand);
			}
		}
		else{//beacon launch or kill

			if ( _beacons_Name2Channel.find( channelName ) != _beacons_Name2Channel.end() ){

				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
			else{
				std::shared_ptr< BeaconChannel > newChannel( new BeaconChannel(channelName, _globalVars) );
				_beacons_Name2Channel[channelName] = newChannel;
				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
		}
	}
	else if ( current -> identify() == "MessageReceive" ){

		MessageReceiveBlock *mrb = dynamic_cast< MessageReceiveBlock * >( current );
		std::string channelName = substituteChannelName( mrb -> getChannelName(), currentParameters.intValues );

		if ( mrb -> isHandshake() ){

			std::shared_ptr< Candidate > cand( new Candidate( mrb, currentParameters, sp -> localVariables, sp, parallelProcesses) );
			std::set<int> setEval = evalRPN_set( mrb -> getSetExpression(), currentParameters, _globalVars, sp -> localVariables );

			cand -> rangeEvaluation = setEval;

			if ( _handshakes_Name2Channel.find( channelName ) != _handshakes_Name2Channel.end() ){

				_handshakes_Name2Channel[channelName] -> addReceiveCandidate(cand);
			}
			else{
				std::shared_ptr< HandshakeChannel > newChannel( new HandshakeChannel(channelName, _globalVars) );
				_handshakes_Name2Channel[channelName] = newChannel;
				_handshakes_Name2Channel[channelName] -> addReceiveCandidate(cand);
			}
		}
		else if ( mrb -> isCheck() ){

			if ( _beacons_Name2Channel.find( channelName ) != _beacons_Name2Channel.end() ){

				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
			else{
				std::shared_ptr< BeaconChannel > newChannel( new BeaconChannel(channelName, _globalVars) );
				_beacons_Name2Channel[channelName] = newChannel;
				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
		}
		else{//beacon receive

			if ( _beacons_Name2Channel.find( channelName ) != _beacons_Name2Channel.end() ){

				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
			else{
				std::shared_ptr< BeaconChannel > newChannel( new BeaconChannel(channelName, _globalVars) );
				_beacons_Name2Channel[channelName] = newChannel;
				_beacons_Name2Channel[channelName] -> addCandidate( current, sp, parallelProcesses, currentParameters, _candidatesLeft, _rateSum );
			}
		}
	}
	else if ( current -> identify() == "Gate" ){

		GateBlock *gb = dynamic_cast< GateBlock * >( current );
		bool gateConditionHolds = evalRPN_condition( gb -> getConditionExpression(), currentParameters, _globalVars, sp -> localVariables );
		if ( gateConditionHolds ){

			std::vector< Block * > children = bt.getChildren(current);
			assert( children.size() == 1 );//gates are unary
			sumTransitionRates( sp, bt, children[0], parallelProcesses, currentParameters );
		}
	}
	else if ( current -> identify() == "Process" ){

		ProcessBlock *pb = dynamic_cast< ProcessBlock * >(current);

		//update the parameter values based on any process arithmetic we're doing
		ParameterValues oldParameterValues = currentParameters;
		std::vector< std::string > parameterNames = _name2ProcessDef[ pb -> getProcessName()].parameters;
		for ( unsigned int i = 0; i < parameterNames.size(); i++ ){

			//check literals, parameter values, and global variables in the expression to see if we should cast this to a double
			if ( castToDouble(pb -> getParameterExpressions()[i], _globalVars, oldParameterValues ) ){

				currentParameters.updateValue( parameterNames[i], evalRPN_double(pb -> getParameterExpressions()[i], oldParameterValues , _globalVars, sp -> localVariables) );
			}
			else{

				currentParameters.updateValue( parameterNames[i], evalRPN_int(pb -> getParameterExpressions()[i], oldParameterValues , _globalVars, sp -> localVariables) );
			}
		}
		//recurse down using this process's tree and the updated parameter values
		Tree<Block> newTree = _name2ProcessDef[ pb -> getProcessName()].parseTree;
		sumTransitionRates( sp, newTree, newTree.getRoot(), parallelProcesses, currentParameters );
	}
	else if ( current -> identify() == "Parallel" ){

		std::vector< Block * > children = bt.getChildren( current );

		//left child
		std::list< SystemProcess > forLeft = parallelProcesses;
		SystemProcess left_sp = SystemProcess( *sp );
		left_sp.parseTree = bt.getSubtree( children[1] );
		forLeft.push_back( left_sp );
		sumTransitionRates( sp, bt, children[0], forLeft, currentParameters );

		//right child
		SystemProcess right_sp = SystemProcess( *sp );
		right_sp.parseTree = bt.getSubtree( children[0] );
		parallelProcesses.push_back( right_sp );
		sumTransitionRates( sp, bt, children[1], parallelProcesses, currentParameters );
	}
	else {
		std::vector< Block * > children = bt.getChildren(current);
		for ( auto c = children.begin(); c < children.end(); c++ ){

			sumTransitionRates( sp, bt, *c, parallelProcesses, currentParameters );
		}
	}
}


void System::getParallelProcesses( std::shared_ptr<Candidate> chosen, std::list< SystemProcess * > &toAdd ){
//if we choose this candidate, get the processes that would act in parallel to this one

	//add parallel processes to the system
	for ( auto pp = (chosen -> parallelProcesses).begin(); pp != (chosen -> parallelProcesses).end(); pp++ ){

		toAdd.push_back( new SystemProcess( *pp ) );
	}
}


SystemProcess * System::updateSpForTransition( std::shared_ptr<Candidate> chosen ){

	SystemProcess *SPtoModify = chosen -> processInSystem;

	//get the child of the chosen action, and update the current system process so that it starts from there
	Block *actionDone = chosen -> actionCandidate;
	Tree<Block> treeForAction = _name2ProcessDef[ actionDone -> getOwningProcess() ].parseTree;

	if ( treeForAction.isLeaf( actionDone ) ) return NULL;
	else {

		SystemProcess *newSp = new SystemProcess( *SPtoModify );
		newSp -> parameterValues = chosen -> parameterValues; //inherit the parameter variables from the candidate
		std::vector< Block * > children = treeForAction.getChildren( actionDone );
		assert( children.size() == 1 );
		newSp -> parseTree = treeForAction.getSubtree( children[0] );
#if DEBUG
std::cout << "kill sp " << SPtoModify << " and add " << newSp << std::endl;
#endif
		return newSp;
	}
}


void System::splitOnParallel(SystemProcess *sp, Block *currentNode, std::list< SystemProcess * > &toAdd ){
//recurse down a parse tree, get the first blocks that aren't parallel operators, and make separate system processes for them
//prevents issues in situations where we have handshakes between two message actions within a single system process

	if ( currentNode -> identify() == "Parallel" ){

		std::vector< Block * > children = (sp -> parseTree).getChildren( currentNode );
		for ( auto c = children.begin(); c < children.end(); c++ ){

			splitOnParallel(sp, *c, toAdd );
		}
	}
	else {

		SystemProcess *newSp = new SystemProcess( *sp );
		newSp -> parseTree = (sp -> parseTree).getSubtree( currentNode );
		toAdd.push_back( newSp );
		return;
	}
}


void System::removeChosenFromSystem( std::shared_ptr<Candidate> candToRemove ){

	SystemProcess *sp = candToRemove -> processInSystem;

	//take away all the rates that this system contributed to the rateSum
	bool inNonMsgCandidates = false;
	for ( auto c = _nonMsgCandidates[sp].begin(); c != _nonMsgCandidates[sp].end(); c++ ){

		_rateSum -= (*c) -> rate;
		_candidatesLeft--;
		inNonMsgCandidates = true;
	}

	//erase from candidates
	if ( inNonMsgCandidates ) _nonMsgCandidates.erase( _nonMsgCandidates.find( sp ) );

	//erase any beacon candidate that pertains to sp
	for ( auto be = _beacons_Name2Channel.begin(); be != _beacons_Name2Channel.end(); be++ ){

		(be -> second) -> cleanSPFromChannel(sp,_candidatesLeft,_rateSum);
	}

	//erase any handshake candidate that could be sent or received from sp
	for ( auto hs = _handshakes_Name2Channel.begin(); hs != _handshakes_Name2Channel.end(); hs++ ){

		int handshakesRemoved;
		double rateSumDecrease; 
		std::tie(handshakesRemoved,rateSumDecrease) = (hs -> second) -> cleanSPFromChannel(sp);
		_rateSum -= rateSumDecrease;
		_candidatesLeft -= handshakesRemoved;
	}

	//reshuffle potential vs active beacon receives
	for ( auto be = _beacons_Name2Channel.begin(); be != _beacons_Name2Channel.end(); be++ ){

		(be -> second) -> updateBeaconCandidates(_candidatesLeft,_rateSum);
	}

	//remove the system process from the system
	_currentProcesses.erase( std::find(_currentProcesses.begin(), _currentProcesses.end(), sp ) ); 
#if DEBUG
std::cout << "deleting system process pointer: " << sp << std::endl;
#endif
	delete sp;
}


void System::simulate(void){

	while ( _candidatesLeft > 0 and _transitionsTaken < _maxTransitions and _totalTime <= _maxDuration ){

#if DEBUG
std::cout << "-----------------" << std::endl;
std::cout << "Candidates left: " << _candidatesLeft << std::endl;
std::cout << "Transitions taken: " << _transitionsTaken << std::endl;
#endif

		/*draw time of next transition */
		std::random_device rd;
		std::mt19937 rnd_gen( rd() );
		std::exponential_distribution< double > expDist(_rateSum);
		double exponentialDraw = expDist(rnd_gen);
		_totalTime += exponentialDraw;

		/*monte carlo step to decide next transition */
		std::uniform_real_distribution< double > uniDist(0.0, 1.0);
		double uniformDraw = uniDist(rnd_gen);

		/*go through all the transition candidates and stop when we find the correct one */
		double runningTotal = 0.0;
		bool found = false;
		std::list< SystemProcess * > toAdd;

		/*non-messaging choice */
		for ( auto s = _nonMsgCandidates.begin(); s != _nonMsgCandidates.end(); s++ ){

			for ( auto tc = (s -> second).begin(); tc < (s -> second).end(); tc++ ){

				double lower = runningTotal / _rateSum;
				double upper = (runningTotal + ( (*tc) -> rate)) / _rateSum;

				if ( uniformDraw > lower and uniformDraw <= upper ){
#if DEBUG
std::cout << "picked non-msg action" << std::endl;
#endif
					//designate the chosen one
					getParallelProcesses( *tc, toAdd );
					SystemProcess *newSp = updateSpForTransition( *tc );
					if ( newSp ) toAdd.push_back(newSp);
					writeTransition( _totalTime, *tc, _outputStream );
					removeChosenFromSystem(*tc);
					found = true;
					goto foundCand;
				}
				else runningTotal += (*tc) -> rate;
			}
		}

		/*if we haven't chosen from the non-messaging choices, look at beacon action */
		for ( auto chanPair = _beacons_Name2Channel.begin(); chanPair != _beacons_Name2Channel.end(); chanPair++ ){ 
			
			std::shared_ptr<Candidate> beaconCand = (chanPair -> second) -> pickCandidate(runningTotal, uniformDraw, _rateSum);
			if ( beaconCand != NULL ){

#if DEBUG
std::cout << "picked a beacon" << std::endl;
#endif

				getParallelProcesses( beaconCand, toAdd );
				SystemProcess *newSp = updateSpForTransition( beaconCand );

				if ( newSp and (beaconCand -> actionCandidate) -> identify() == "MessageReceive" ){

					//bind a new variable if applicable
					MessageReceiveBlock *mrb = dynamic_cast< MessageReceiveBlock * >( beaconCand -> actionCandidate );
					if ( mrb -> bindsVariable() ){

						assert( (beaconCand -> rangeEvaluation).size() == 1 );
						newSp -> localVariables[ mrb -> getBindingVariable() ] = *( (beaconCand -> rangeEvaluation).begin() );
					}
				}

				if ( newSp ) toAdd.push_back(newSp);
				writeTransition( _totalTime, beaconCand, _outputStream );
				removeChosenFromSystem(beaconCand);
				found = true;
				goto foundCand;
			}
		}

		/*if we haven't chosen from the beacon actions, look at the handshakes */
		for ( auto chanPair = _handshakes_Name2Channel.begin(); chanPair != _handshakes_Name2Channel.end(); chanPair++ ){ 
			
			std::shared_ptr<HandshakeCandidate> hsCand = (chanPair -> second) -> pickCandidate(runningTotal, uniformDraw, _rateSum);
			
			if ( hsCand != NULL ){

#if DEBUG
std::cout << "picked a handshake" << std::endl;
#endif

				//handshake send
				getParallelProcesses( hsCand -> hsSendCand, toAdd );
				SystemProcess *newSp_send = updateSpForTransition( hsCand -> hsSendCand );
				if ( newSp_send ) toAdd.push_back(newSp_send);

				//handshake receive
				getParallelProcesses( hsCand -> hsReceiveCand, toAdd );
				SystemProcess *newSp_receive = updateSpForTransition( hsCand -> hsReceiveCand ); //issue here

				if ( newSp_receive ){
					//bind a new variable if applicable
					MessageReceiveBlock *mrb = dynamic_cast< MessageReceiveBlock * >( (hsCand -> hsReceiveCand) -> actionCandidate );
					if ( mrb -> bindsVariable() ){

						newSp_receive -> localVariables[ mrb -> getBindingVariable() ] = hsCand -> receivedParam;
					}
					toAdd.push_back(newSp_receive);
				}

				writeTransition( _totalTime, hsCand -> hsSendCand, _outputStream );
				writeTransition( _totalTime, hsCand -> hsReceiveCand, _outputStream );

				//remove handshake from the system
				removeChosenFromSystem( hsCand -> hsSendCand );
				removeChosenFromSystem( hsCand -> hsReceiveCand );

				found = true;
				goto foundCand;
			}
		}

		foundCand:
		assert( found );
		_transitionsTaken++;

#if DEBUG
std::cout << "reformatting" << std::endl;
#endif

		//re-format the system
		std::list< SystemProcess * > newProcesses;
		for ( auto s = toAdd.begin(); s != toAdd.end(); s++ ){

			//see if we can make multiple system processes out of this one by splitting on parallel operators
			if ( ( (*s) -> parseTree).getRoot() -> identify() == "Parallel" ){				

				splitOnParallel( *s, ((*s) -> parseTree).getRoot(), newProcesses );
				delete *s;
				s = _currentProcesses.erase( s );
			}
		}
		toAdd.insert( toAdd.end(), newProcesses.begin(), newProcesses.end() );

#if DEBUG
std::cout << "re-summing transitions" << std::endl;
#endif

		//sum the transition rates for non-handshake candidates while buildling a list of candshake candidates
		std::list< SystemProcess > parallelProcesses;
		for ( auto s = toAdd.begin(); s != toAdd.end(); s++ ){

			sumTransitionRates( *s, (*s) -> parseTree, ((*s) -> parseTree).getRoot(), parallelProcesses, (*s) -> parameterValues );
		}

#if DEBUG
std::cout << "re-summing handshakes" << std::endl;
#endif

		//sum handshake transitions
		for ( auto chan = _handshakes_Name2Channel.begin(); chan != _handshakes_Name2Channel.end(); chan++ ){

			int newHandshakesAdded;
			double rateSumIncrease;
			std::tie(newHandshakesAdded,rateSumIncrease) = (chan -> second) -> updateHandshakeCandidates();
			_candidatesLeft += newHandshakesAdded;
			_rateSum += rateSumIncrease;
		}
		_currentProcesses.insert( _currentProcesses.end(), toAdd.begin(), toAdd.end() );
	}
}


void simulateSystem( std::map< std::string, ProcessDefinition > &name2ProcessDef, std::list< SystemProcess > &system, GlobalVariables &globalVars, int numOfSimulations, int threads, std::string outputFilename, int maxTransitions, double maxDuration ){

	std::ofstream outFile( outputFilename );
	progressBar pb( numOfSimulations );
	int numCompleted = 0;

	/*each simulation */
	#pragma omp parallel for schedule(dynamic) shared(pb, system, globalVars, numCompleted) num_threads( threads )
	for ( int i = 0; i < numOfSimulations; i++ ){

		System systemLocal( system, name2ProcessDef, maxTransitions, maxDuration, globalVars );
		systemLocal.simulate();
		numCompleted++;

		#pragma omp critical 
		{
		pb.displayProgress( numCompleted );
		outFile << ">=======" << std::endl;
		outFile << systemLocal.write();
		}
	}
	std::cout << std::endl;
}